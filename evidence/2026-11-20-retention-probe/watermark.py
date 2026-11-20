"""Exercise evrec's actual disk_allow function with synthetic free space and monotonic time."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).read_text()
start = source.index("static int disk_allow(void)\n{")
end = source.index("\n}", start) + 2
values = {key:int(re.search(r"static int g_disk_"+key+r" = ([0-9]+);",source)[1])
          for key in ("min_mb","hyst_mb")}
program = r'''
#include <stdio.h>
#include <time.h>
static int g_disk_stopped, queries;
static double g_disk_next_check, tick;
static long available;
static char g_disk_dir[] = "fixture";
static long disk_free_mb(void) { ++queries; return available; }
static double now_ms(int clock) { (void)clock; return tick; }
'''
program += "\n".join("static int g_disk_%s=%d;" % item for item in values.items())
program += "\n"+source[start:end]+r'''
int main(void) {
    const int times[] = {0,10000,11000,20000,30000,40000};
    const int space[] = {1000,999,2000,1999,2000,-1};
    const int allowed[] = {1,0,0,0,1,1};
    for (int i=0;i<6;i++) {
        tick=times[i];
        available=space[i];
        int result=disk_allow();
        printf("sample %d %d %d %d\n",times[i],space[i],result,queries);
        if (result!=allowed[i]) return 1;
    }
    return queries==5 ? 0 : 1;
}
'''
with tempfile.TemporaryDirectory(prefix="watermark-probe-") as name:
    folder=Path(name)
    (folder/"probe.c").write_text(program)
    subprocess.run(["cc","-Wall","-Werror","probe.c","-o","probe"],cwd=folder,check=True)
    lines=subprocess.check_output([str(folder/"probe")],text=True).splitlines()
rows=[dict(zip(("ms","offered_mib","allows_recording","space_queries"),map(int,l.split()[1:])))
      for l in lines if l.startswith("sample ")]
print(json.dumps(dict(config=values, rows=rows),indent=2))
