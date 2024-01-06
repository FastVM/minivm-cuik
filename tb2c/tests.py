import pathlib
import subprocess
import sys
import os
import tempfile
import json

# files_to_remove = []
# num_gen_files = 0
# def gen_file(dir: str, ext: str = '.tmp'):
#     global num_gen_files
#     num_gen_files += 1
#     ret = pathlib.Path(dir) / f'out{num_gen_files}.{ext}'
#     files_to_remove.append(ret)
#     return ret

def default_test_dir() -> str:
    return str(pathlib.Path(__file__).parent.resolve() / 'tests')

def cuik_file_to_c(in_file_name: str, out_file_name: str, opt: list[str] = []) -> None:
    with open(out_file_name, 'w') as out_file:
        subprocess.call(
            ['bin/cuik', '-emit-c', in_file_name],
            stdout = out_file,
        )

def read(name):
    with open(name, 'r') as f:
        return f.read()

def cc_results(cc: str, in_file_name: str, opt: list[str] = []):
    with tempfile.TemporaryDirectory() as cc_outdir:
        cc_stdout = str(pathlib.Path(cc_outdir) / 'cc.out.log')
        cc_stderr = str(pathlib.Path(cc_outdir) / 'cc.err.log')
        cc_outfile = str(pathlib.Path(cc_outdir) / 'out.exe')
        with open(cc_stdout, 'w') as out_w:
            with open(cc_stderr, 'w') as err_w:
                cc_code = subprocess.call(
                    [cc, in_file_name, '-w', *opt, '-o', cc_outfile],
                    stdout = out_w,
                    stderr = err_w,
                )
        run_stdout = str(pathlib.Path(cc_outdir) / 'run.out.log')
        run_stderr = str(pathlib.Path(cc_outdir) / 'run.err.log')
        with open(run_stdout, 'w') as out_w:
            with open(run_stderr, 'w') as err_w:
                run_code = subprocess.call(
                    [cc_outfile],
                    stdout = out_w,
                    stderr = err_w,
                )
        return {
            'cc': {
                'ret': cc_code,
                'out': read(cc_stdout),
                'err': read(cc_stderr),
            },
            'run': {
                'ret': run_code,
                'out': read(run_stdout),
                'err': read(run_stderr),
            }
        }
    
def run_eq(a, b):
    return a['ret'] == b['ret'] and a['out'] == b['out'] and a['err'] == b['err']

def res_eq(a, b):
    return run_eq(a['cc'], b['cc']) and run_eq(a['run'], b['run'])

tmp_files = []

def compile_all_tests(in_dir_name: str, cc: str = 'cc', cuik_opt: list[str] = [], cc_opt: list[str] = []) -> None:
    c_files = list(pathlib.Path(in_dir_name).rglob('*.c'))
    for test_file_path in c_files:
        if test_file_path.parts[-1].endswith('.tmp.c'):
            continue
        test_file_name = str(test_file_path)
        out_c_file_name =  f'{test_file_name}.tmp.c'
        tmp_files.append(out_c_file_name)
        cuik_file_to_c(
            in_file_name = test_file_name,
            out_file_name = out_c_file_name,
            opt = cuik_opt,
        )
        gcc_res = cc_results(
            cc = cc,
            in_file_name = test_file_name,
            opt = cc_opt,
        )
        cuik_gcc_res = cc_results(
            cc = cc,
            in_file_name = out_c_file_name,
            opt = cc_opt,
        )
        test_name = test_file_name.removeprefix(in_dir_name).removeprefix('/')
        if res_eq(gcc_res, cuik_gcc_res):
            print('pass', test_name)
        else:
            print('fail', test_name)
            
        # cuik_res = cuik_results(
        #     cc = str(pathlib.Path(__file__).parent.parent / 'bin' / 'cuik'),
        #     in_file_name = test_file_name,
        # )
        # print(json.dumps(gcc_res, indent = 4))
        # print(json.dumps(cuik_gcc_res, indent = 4))

def main(args):
    test_dir = default_test_dir()
    compile_all_tests(test_dir)
    for file in tmp_files:
        os.remove(file)

if __name__ == '__main__':
    main(sys.argv)
