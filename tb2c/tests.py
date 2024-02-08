import pathlib
import subprocess
import sys
import os
import tempfile
import time
import enum
import dataclasses
import shutil

def default_test_dir() -> str:
    return str(pathlib.Path(__file__).parent.resolve() / 'tests')

@dataclasses.dataclass
class CommandResult:
    return_code: int
    stdout: str
    stderr: str
    seconds: float = dataclasses.field(compare = False)

    @staticmethod
    def run(*args) -> 'CommandResult':
        with tempfile.NamedTemporaryFile() as out_w:
            with tempfile.NamedTemporaryFile() as err_w:
                start = time.time()
                try:
                    return_code = subprocess.call(
                        args,
                        stdout = out_w,
                        stderr = err_w,
                    )
                except FileNotFoundError:
                    return_code = -1
                end = time.time()
                stdout = out_w.read()
                stderr = err_w.read()
        return CommandResult(
            return_code = return_code,
            stdout = stdout,
            stderr = stderr,
            seconds = end - start,
        )
    
@dataclasses.dataclass
class CompileAndRun:
    compile: CommandResult
    run: CommandResult

    @staticmethod
    def cc(cc: str, file: str, opt: list[str] = []) -> 'CompileAndRun':
        with tempfile.TemporaryDirectory() as cc_outdir:
            cc_outfile = str(pathlib.Path(cc_outdir) / 'out.exe')
            cc_res = CommandResult.run(cc, file, '-w', '-o', cc_outfile, *opt)
            run_res = CommandResult.run(cc_outfile)
            return CompileAndRun(
                compile = cc_res,
                run = run_res,
            )

@enum.unique
class ArgMode(enum.Enum):
    BASE = enum.auto()
    FLAG_C = enum.auto()
    FLAG_XCC = enum.auto()
    FLAG_XCUIK = enum.auto()
    FLAG_F = enum.auto()
    FLAG_N = enum.auto()

class ArgError(Exception):
    pass

class Args:
    c: str
    n: int
    xcc: str
    xcuik: str
    clean: bool
    file_dir: str

    __slots__ = ('c', 'n', 'xcc', 'xcuik', 'clean', 'file_dir')

    def __init__(self, argv0: str, *args: list[str]):
        self.c = 'cc'
        self.n = 1
        self.xcc = []
        self.xcuik = []
        self.clean = False
        self.file_dir = default_test_dir()
        mode: ArgMode = ArgMode.BASE
        for arg in args:
            match mode:
                case ArgMode.BASE:
                    match arg:
                        case '-n':
                            mode = ArgMode.FLAG_N
                        case '-c':
                            mode = ArgMode.FLAG_C
                        case '-Xcc':
                            mode = ArgMode.FLAG_XCC
                        case '--no-clean':
                            self.clean = False
                        case '-f':
                            mode = ArgMode.FLAG_F
                        case '--clean':
                            self.clean = True
                        case '-Xcuik':
                            mode = ArgMode.FLAG_XCUIK
                        case _:
                            raise ArgError(f'unknown arg: {arg}')
                case ArgMode.FLAG_N:
                    self.n = int(arg)
                    mode = ArgMode.BASE
                case ArgMode.FLAG_C:
                    self.c = arg
                    mode = ArgMode.BASE
                case ArgMode.FLAG_XCC:
                    self.xcc.append(arg)
                    mode = ArgMode.BASE
                case ArgMode.FLAG_XCUIK:
                    self.xcuik.append(arg)
                    mode = ArgMode.BASE
                case ArgMode.FLAG_F:
                    self.file_dir = arg
                    mode = ArgMode.BASE

class Tests:
    args: Args
    tmp_files: list[str]
    def __init__(self, argv: list[str]):
        self.args = Args(*argv)
        self.tmp_files = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, exc_tb):
        if self.args.clean:
            for file in self.tmp_files:
                os.remove(file)

    def cuik_to_c(self, infile: str, outfile: str) -> None:
        with open(outfile, 'w') as out_file:
            subprocess.call(
                ['bin/cuik', '-emit-c', infile, *self.args.xcuik],
                stdout = out_file,
            )

    def repeat_cuik_to_c(self, infile: str, outfile: str, n: int):
        if n == 0:
            shutil.copy(infile, outfile)
        elif n == 1:
            self.cuik_to_c(infile, outfile)
        else:
            mid_path = f'{outfile}.tmp.c'
            self.repeat_cuik_to_c(infile, mid_path, n = n-1)
            self.cuik_to_c(mid_path, outfile)
                

    def all(self) -> None:
        c_files = list(pathlib.Path(self.args.file_dir).rglob('*.c'))
        c_files.sort()
        for test_file_path in c_files:
            if test_file_path.parts[-1].endswith('.tmp.c'):
                continue
            c_in = str(test_file_path)
            c_out = c_in.removesuffix('.c') + '.tmp.c'
            self.tmp_files.append(c_out)
            self.repeat_cuik_to_c(
                infile = c_in,
                outfile = c_out,
                n = self.args.n,
            )
            cc_res = CompileAndRun.cc(
                cc = self.args.c,
                file = c_in,
                opt = self.args.xcc,
            )
            cuik_cc_res = CompileAndRun.cc(
                cc = self.args.c,
                file = c_out,
                opt = self.args.xcc,
            )
            test_name = test_file_path.parts[-1]
            if cc_res == cuik_cc_res:
                cc_msec = cc_res.run.seconds * 1000
                cuik_cc_msec = cuik_cc_res.run.seconds * 1000
                print(f'pass {test_name} (cuik -> {self.args.c}: {cuik_cc_msec:.1f}ms) ({self.args.c}: {cc_msec:.3f}ms)')
            else:
                print('fail', test_name)
            
def main(argv):
    with Tests(argv) as tests:
        tests.all()

if __name__ == '__main__':
    main(sys.argv)
