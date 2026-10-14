"""Audit-only handoff of a verified suspended dispatcher to two extra workers.

Never stops native solvers. The caller verifies both Python PIDs and command
lines; this helper takes ownership of the existing pause and always releases it.
"""
import argparse, concurrent.futures, ctypes, time
from pathlib import Path
from replay import jobs, execute, read, save

def main(out,parent,previous):
    journal=read(out/'queue_barrier.json')
    assert journal['pid']==parent and journal['log'][-1]['status']=='dispatcher_paused_native_solvers_continue'
    assert not (out/'parallel_population/complete.json').exists()
    k=ctypes.WinDLL('kernel32',use_last_error=True);nt=ctypes.WinDLL('ntdll')
    k.OpenProcess.argtypes=[ctypes.c_ulong,ctypes.c_int,ctypes.c_ulong];k.OpenProcess.restype=ctypes.c_void_p
    k.TerminateProcess.argtypes=[ctypes.c_void_p,ctypes.c_uint];k.TerminateProcess.restype=ctypes.c_int
    k.CloseHandle.argtypes=[ctypes.c_void_p]
    nt.NtResumeProcess.argtypes=[ctypes.c_void_p];nt.NtResumeProcess.restype=ctypes.c_long
    dispatcher=k.OpenProcess(0x1800,False,parent)
    old=k.OpenProcess(1,False,previous)
    assert dispatcher and old,'Cannot open verified audit helpers'
    log=[];owned=False
    def mark(status):
        log.append(dict(status=status,epoch=time.time()))
        save(out/'queue_handoff.json',dict(parent=parent,previous=previous,log=log));print(status,flush=True)
    try:
        # Previous helper has no native child. Transfer its existing hold.
        if not k.TerminateProcess(old,0):raise ctypes.WinError(ctypes.get_last_error())
        owned=True;mark('pause_owned_previous_helper_stopped')
        plan=[j for j in jobs() if j['group']=='lab3_network_convergence' and j['name'] in
              ('network_benchmark_quarter','network_benchmark_sixteenth')]
        assert len(plan)==2
        assert all(not (out/j['group']/'running'/(j['name']+'.json')).exists() for j in plan)
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results=list(pool.map(lambda j:execute(j,out),plan))
        assert all(r['complete'] for r in results)
        mark('two_queued_benchmarks_complete')
        deadline=time.time()+14400
        while time.time()<deadline:
            marker=out/'parallel_population/complete.json'
            if marker.exists() and read(marker)['complete']:break
            time.sleep(2)
        else:raise TimeoutError('Population publication watchdog expired')
        mark('population_batch_complete')
    finally:
        if owned:
            status=nt.NtResumeProcess(dispatcher)
            if status:raise RuntimeError(f'Resume dispatcher {parent}: status {status}')
            mark('dispatcher_resumed')
        k.CloseHandle(old);k.CloseHandle(dispatcher)

if __name__=='__main__':
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True,type=Path)
    ap.add_argument('--parent',required=True,type=int);ap.add_argument('--previous',required=True,type=int)
    a=ap.parse_args();main(a.output.resolve(),a.parent,a.previous)
