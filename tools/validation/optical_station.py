#!/usr/bin/env python3
"""Visible reference clock + RTSP viewer for external optical recording.

The software records draw requests and decode availability, never photon times.
Point the RK camera only at the left reference area; an external camera must see
both the direct reference and the returned RTSP image to measure display delay.
"""
import argparse
import csv
import json
from pathlib import Path
import queue
import sys
import threading
import time
import tkinter as tk

from PIL import Image, ImageDraw, ImageFont, ImageTk

PERIOD_MS = 1_000_000
CONTRAST_HOLDS_MS=(900,1300,800,1600,1000,700,1400,1100,1700,900,1200,1500)

def contrast_phase(elapsed_ms):
    remaining=elapsed_ms % sum(CONTRAST_HOLDS_MS)
    for index,duration in enumerate(CONTRAST_HOLDS_MS):
        if remaining<duration:
            return index%2
        remaining-=duration
    raise AssertionError('unreachable contrast phase')

def reference_image(counter_ms, width=760, height=430):
    image = Image.new('RGB', (width, height), '#111827')
    draw = ImageDraw.Draw(image)
    try:
        large = ImageFont.truetype('consola.ttf', int(width*.18))
        small = ImageFont.truetype('consola.ttf', int(width*.035))
    except OSError:
        large = ImageFont.load_default(size=int(width*.18))
        small = ImageFont.load_default(size=int(width*.035))
    value = counter_ms % PERIOD_MS
    draw.text((width*.04, height*.03), 'REFERENCE / milliseconds', fill='white', font=small)
    draw.text((width*.04, height*.22), '%06d' % value, fill='white', font=large)
    draw.text((width*.04, height*.63), 'RK camera sees LEFT area only', fill='#a5b4fc', font=small)
    # A 20-bit value and its complement aid rejection of torn/mixed images.
    bits = [(value >> i) & 1 for i in range(19, -1, -1)]
    for row, values in enumerate((bits, [1-b for b in bits])):
        for i, bit in enumerate(values):
            x0, x1 = int(i*width/20), int((i+1)*width/20)
            y0 = int(height*.79) + row*int(height*.09)
            draw.rectangle((x0, y0, x1-2, y0+int(height*.07)), fill='white' if bit else 'black')
    return image

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--url', help='RTSP stream; absent for synthetic layout check')
    p.add_argument('--av-site', type=Path, help='isolated PyAV site-packages; host Pillow/NumPy remain explicit dependencies')
    p.add_argument('--duration', type=int, default=60)
    p.add_argument('--headless', action='store_true', help='software decode readiness only, no window or physical timing')
    p.add_argument('--reconnect', action='store_true', help='bounded preparation window may reconnect; all failed attempts retained')
    p.add_argument('--pattern',choices=('counter','contrast'),default='counter')
    p.add_argument('--geometry',default='1580x660+40+40',help='initial Tk geometry; keep fixed during optical recording')
    a = p.parse_args()
    if a.av_site:
        sys.path.insert(0,str(a.av_site.resolve()))
    if not 5 <= a.duration <= 600:
        p.error('duration must be 5..600 seconds')
    out = a.out.absolute()
    out.mkdir(mode=0o700)
    (out/'control').mkdir()
    stop = threading.Event()
    latest = queue.Queue(maxsize=1)
    data = {'mode': 'rtsp' if a.url else 'synthetic-layout-only', 'url': a.url,
            'decoded': 0, 'queue_replaced': 0, 'decoder_error': None,
            'scope': 'software draw requests/decode availability; physical display latency NOT measured'}
    data['connection_attempts']=[]
    data['sampling_markers']={}
    data['pattern']=a.pattern
    data['contrast_hold_schedule_ms']=CONTRAST_HOLDS_MS if a.pattern=='contrast' else None
    origin = time.perf_counter_ns()
    draw_rows, decode_rows, display_rows = [], [], []

    def decode():
        import av
        data['pyav'] = av.__version__
        data['ffmpeg_libraries'] = av.library_versions
        while not stop.is_set():
            data['open_requested_ns'] = time.perf_counter_ns()
            attempt={'open_requested_ns':data['open_requested_ns'],'error':None}
            data['connection_attempts'].append(attempt)
            try:
                with av.open(a.url, options={'rtsp_transport':'tcp'}, timeout=(5,5)) as container:
                    stream = container.streams.video[0]
                    data['codec'] = stream.codec_context.name
                    data['decoder_error']=None
                    for frame in container.decode(stream):
                        when = time.perf_counter_ns()
                        if stop.is_set():
                            break
                        picture = frame.to_image().resize((760,430), Image.Resampling.BILINEAR)
                        data['decoded'] += 1
                        decode_rows.append((data['decoded'], when, frame.pts, str(frame.time_base)))
                        item=(data['decoded'],when,picture)
                        try:
                            latest.put_nowait(item)
                        except queue.Full:
                            try:
                                latest.get_nowait()
                                data['queue_replaced'] += 1
                            except queue.Empty:
                                pass
                            latest.put_nowait(item)
            except Exception as exc:
                attempt['error']=type(exc).__name__+': '+str(exc)
                data['decoder_error']=attempt['error']
            attempt['finished_ns']=time.perf_counter_ns()
            if not a.reconnect:
                break
            stop.wait(1)

    if a.headless:
        if not a.url:
            p.error('--headless requires --url')
        worker=threading.Thread(target=decode,daemon=True)
        worker.start()
        until=time.monotonic()+a.duration
        while time.monotonic()<until and data['decoded']<30 and not data['decoder_error']:
            try:
                latest.get(timeout=.1)
            except queue.Empty:
                pass
        stop.set()
        worker.join(timeout=7)
        with (out/'decode.csv').open('x',newline='',encoding='utf-8') as stream:
            writer=csv.writer(stream)
            writer.writerow(('decoded_index','decode_available_ns','pts','time_base'))
            writer.writerows(decode_rows)
        data['worker_still_alive']=worker.is_alive()
        data['av_open_to_first_decoded_frame_ms']=(decode_rows[0][1]-data['open_requested_ns'])/1e6 if decode_rows else None
        data['startup_metric_scope']='av.open call start to first decoded frame availability; includes handshake/probing; not photon/display latency or frame freshness'
        data['software_decode_ready']=data['decoded']>=30 and not data['decoder_error'] and not worker.is_alive()
        data['optical_measurement_status']='NOT_MEASURED; headless decode only'
        (out/'result.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(data),flush=True)
        return 0 if data['software_decode_ready'] else 1

    window = tk.Tk()
    window.title('RK3568 Optical Latency Station')
    window.geometry(a.geometry)
    window.configure(bg='#111827')
    title = tk.Label(window, text='LEFT: direct reference       RIGHT: returned video | external camera records BOTH',
                     fg='white', bg='#111827', font=('Segoe UI',16))
    title.pack(pady=14)
    phase=tk.Label(window,text='PREPARE ONLY - do not record yet',fg='#fbbf24',bg='#111827',font=('Segoe UI',18,'bold'))
    phase.pack()
    panels = tk.Frame(window, bg='#111827')
    panels.pack()
    left = tk.Canvas(panels,width=760,height=430,highlightthickness=0,bg='#111827')
    left.pack(side='left',padx=8)
    left.background=ImageTk.PhotoImage(reference_image(0))
    left.create_image(0,0,anchor='nw',image=left.background)
    left.create_rectangle(15,85,745,255,fill='#111827',outline='')
    counter_text=left.create_text(30,105,anchor='nw',text='000000',fill='white',font=('Consolas',-125))
    bit_cells=[]
    for row in range(2):
        bit_cells.append([left.create_rectangle(i*38,340+row*38,(i+1)*38-2,369+row*38,
                                                fill='black',outline='') for i in range(20)])
    contrast_cells=[]
    contrast_labels=[]
    if a.pattern=='contrast':
        left.create_rectangle(0,225,760,430,fill='#111827',outline='')
        for x0,x1,label in ((25,365,'A'),(395,735,'B')):
            contrast_cells.append(left.create_rectangle(x0,245,x1,415,fill='#111111',outline='#64748b',width=2))
            contrast_labels.append(left.create_text((x0+x1)/2,330,text=label,font=('Consolas',-65),fill='white'))
        left.create_text(380,230,text='A/B complementary brightness test',fill='#a5b4fc',font=('Consolas',-18))
    right = tk.Label(panels, bg='black')
    right.pack(side='left',padx=8)
    status = tk.Label(window, fg='#a5b4fc', bg='#111827',font=('Segoe UI',13))
    status.pack(pady=12)
    tk.Label(window,text='Software timestamps are not screen photon timestamps. Escape closes this bounded session.',
             fg='white',bg='#111827',font=('Segoe UI',12)).pack()
    window.bind('<Escape>',lambda event:window.destroy())
    worker = threading.Thread(target=decode,daemon=True) if a.url else None
    if worker:
        worker.start()
    last_image = Image.new('RGB',(760,430),'#1f2937')
    last_status_ns=0

    def tick():
        nonlocal last_image,last_status_ns
        now = time.perf_counter_ns()
        elapsed_ms = (now-origin)//1_000_000
        if elapsed_ms >= a.duration*1000:
            window.destroy()
            return
        value=elapsed_ms % PERIOD_MS
        left.itemconfigure(counter_text,text='%06d' % value)
        stimulus=contrast_phase(elapsed_ms) if a.pattern=='contrast' else None
        if a.pattern=='counter':
            for i in range(20):
                bit=(value>>(19-i))&1
                left.itemconfigure(bit_cells[0][i],fill='white' if bit else 'black')
                left.itemconfigure(bit_cells[1][i],fill='black' if bit else 'white')
        else:
            for i in range(2):
                bright=stimulus if i==0 else 1-stimulus
                left.itemconfigure(contrast_cells[i],fill='#e5e7eb' if bright else '#111111')
                left.itemconfigure(contrast_labels[i],fill='#111111' if bright else '#e5e7eb')
        draw_rows.append((len(draw_rows),elapsed_ms,now,time.perf_counter_ns(),stimulus))
        changed=False
        if not a.url and len(draw_rows)%4==1:
            last_image = reference_image(elapsed_ms)
            draw = ImageDraw.Draw(last_image)
            draw.rectangle((0,0,760,35),fill='#991b1b')
            draw.text((8,8),'SYNTHETIC LAYOUT CHECK - NO CAMERA / NO DELAY RESULT',fill='white')
            changed=True
        elif a.url:
            try:
                index, decoded_ns, last_image = latest.get_nowait()
                display_rows.append((index,decoded_ns,time.perf_counter_ns()))
                changed=True
            except queue.Empty:
                pass
        if changed:
            right.photo = ImageTk.PhotoImage(last_image)
            right.configure(image=right.photo)
        age_ms=(now-decode_rows[-1][1])/1e6 if decode_rows else None
        stale=bool(a.url and age_ms is not None and age_ms>1500)
        request=out/'control/sample.request'
        markers=data['sampling_markers']
        if request.exists() and 'requested_ns' not in markers:
            markers.update(requested_ns=now,countdown_end_ns=now+5_000_000_000,sampling_end_ns=now+25_000_000_000)
        if markers:
            if now<markers['countdown_end_ns']:
                phase.configure(text='START PHONE RECORDING NOW - sample begins in %d s'%((markers['countdown_end_ns']-now)//1_000_000_000+1),fg='#fbbf24')
            elif now<markers['sampling_end_ns']:
                phase.configure(text='RECORDING WINDOW - %d s remaining'%((markers['sampling_end_ns']-now)//1_000_000_000+1),fg='#4ade80')
            else:
                markers.setdefault('completed_ns',now)
                phase.configure(text='SAMPLE COMPLETE - stop phone recording and keep original file',fg='#4ade80')
        if stale:
            phase.configure(text='STREAM STALE - DO NOT RECORD',fg='#f87171')
        status.configure(fg='#f87171' if stale else '#a5b4fc',text='mode=%s | decoded=%d | queue replaced=%d | %d s remaining%s%s' %
                         (data['mode'],data['decoded'],data['queue_replaced'],a.duration-int(elapsed_ms/1000),
                          ' | '+data['decoder_error'] if data['decoder_error'] else '',
                          ' | STREAM STALE - DO NOT RECORD' if stale else ''))
        if now-last_status_ns>=500_000_000:
            live={'host_monotonic_ns':now,'geometry':window.geometry(),'decoded':data['decoded'],
                  'last_decode_age_ms':age_ms,'stale':stale,'sampling_markers':markers,
                  'decoder_error':data['decoder_error']}
            temporary=out/'live-status.tmp'
            temporary.write_text(json.dumps(live)+'\n',encoding='utf-8')
            try:
                temporary.replace(out/'live-status.json')
            except OSError:
                pass
            last_status_ns=now
        if (out/'control/stop.request').exists():
            window.destroy()
            return
        window.after(10,tick)

    tick()
    window.mainloop()
    stop.set()
    if worker:
        worker.join(timeout=7)
    data['worker_still_alive'] = bool(worker and worker.is_alive())
    for name, columns, rows in (
        ('reference-draw.csv',('draw_index','counter_ms','callback_ns','tk_configure_complete_ns','stimulus_phase'),draw_rows),
        ('decode.csv',('decoded_index','decode_available_ns','pts','time_base'),decode_rows),
        ('video-draw.csv',('decoded_index','decode_available_ns','tk_draw_request_ns'),display_rows)):
        with (out/name).open('x',newline='',encoding='utf-8') as stream:
            writer=csv.writer(stream)
            writer.writerow(columns)
            writer.writerows(rows)
    data['reference_draw_count'] = len(draw_rows)
    data['video_draw_requests'] = len(display_rows)
    data['elapsed_monotonic_s'] = (time.perf_counter_ns()-origin)/1e9
    data['software_loop_check_pass'] = bool(draw_rows and (not a.url or (data['decoded']>0 and display_rows and
                               not data['decoder_error'] and not data['worker_still_alive'])))
    data['optical_measurement_status'] = 'NOT_MEASURED; needs independent optical recording/annotations'
    (out/'result.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(data),flush=True)
    return 0 if data['software_loop_check_pass'] else 1

if __name__=='__main__':
    raise SystemExit(main())
