"""Windows audit-only barrier: pause the Python dispatcher, never its solvers.

The separate population batch publishes all verified records before the main
dispatcher reaches those same jobs. Holding only the parent preserves running
native calculations and prevents redundant runs. Always resume, even on error;
a four-hour watchdog bounds the hold. The caller must verify ownership/PID.
"""
import argparse,ctypes,time
from pathlib import Path
from replay import read,save

def main(pid,out,threshold):
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);nt=ctypes.WinDLL('ntdll')
    kernel.OpenProcess.argtypes=[ctypes.c_ulong,ctypes.c_int,ctypes.c_ulong];kernel.OpenProcess.restype=ctypes.c_void_p
    kernel.GetExitCodeProcess.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
    kernel.CloseHandle.argtypes=[ctypes.c_void_p]
    for fn in (nt.NtSuspendProcess,nt.NtResumeProcess):fn.argtypes=[ctypes.c_void_p];fn.restype=ctypes.c_long
    handle=kernel.OpenProcess(0x1800,False,pid)
    if not handle:raise ctypes.WinError(ctypes.get_last_error())
    paused=False;deadline=time.time()+14400;log=[]
    def mark(status):
        log.append(dict(status=status,epoch=time.time()));save(out/'queue_barrier.json',dict(pid=pid,threshold=threshold,log=log));print(status,flush=True)
    def alive():
        code=ctypes.c_ulong();return bool(kernel.GetExitCodeProcess(handle,ctypes.byref(code))) and code.value==259
    try:
        mark('waiting_for_main_small_cases')
        while alive() and time.time()<deadline:
            done=out/'parallel_population/complete.json'
            if done.exists() and read(done)['complete']:
                mark('population_batch_complete');break
            progress=out/'progress.json'
            if not paused and progress.exists() and read(progress)['completed']>=threshold:
                status=nt.NtSuspendProcess(handle)
                if status!=0:raise RuntimeError(f'NtSuspendProcess returned {status}')
                paused=True;mark('dispatcher_paused_native_solvers_continue')
            time.sleep(2)
    finally:
        if paused and alive():
            status=nt.NtResumeProcess(handle)
            if status!=0:raise RuntimeError(f'NtResumeProcess returned {status}; resume dispatcher PID {pid}')
            mark('dispatcher_resumed')
        kernel.CloseHandle(handle)
    mark('barrier_finished')

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--pid',type=int,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--threshold',type=int,default=218)
    a=ap.parse_args();main(a.pid,a.output.resolve(),a.threshold)
