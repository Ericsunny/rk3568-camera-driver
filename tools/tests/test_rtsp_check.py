#!/usr/bin/env python3
"""Socketpair regression for coalesced RTSP responses and interleaved data."""
from pathlib import Path
import socket
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rtsp_check import read_response


class ResponseTest(unittest.TestCase):
    def test_play_preserves_first_rtp(self):
        reader, writer = socket.socketpair()
        try:
            packet = b'$\x00\x00\x04test'
            writer.sendall(b'RTSP/1.0 200 OK\r\nCSeq: 4\r\n\r\n' + packet)
            reader.settimeout(1)
            self.assertEqual(read_response(reader)[0], 200)
            self.assertEqual(reader.recv(len(packet)), packet)
        finally:
            reader.close(); writer.close()

    def test_body_does_not_consume_next_response(self):
        reader, writer = socket.socketpair()
        try:
            writer.sendall(b'RTSP/1.0 200 OK\r\nContent-Length: 4\r\n\r\nv=0\nNEXT')
            reader.settimeout(1)
            self.assertEqual(read_response(reader)[2], b'v=0\n')
            self.assertEqual(reader.recv(4), b'NEXT')
        finally:
            reader.close(); writer.close()

    def test_truncated_body_fails(self):
        reader, writer = socket.socketpair()
        try:
            writer.sendall(b'RTSP/1.0 200 OK\r\nContent-Length: 4\r\n\r\nx')
            writer.shutdown(socket.SHUT_WR)
            with self.assertRaises(RuntimeError):
                read_response(reader)
        finally:
            reader.close(); writer.close()


if __name__ == '__main__':
    unittest.main()
