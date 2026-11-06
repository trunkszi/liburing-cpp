"""Compare the target SDK representation against independently compiled kernel UAPI."""
import pathlib
import subprocess
import shutil
import sys
import tempfile

headers = pathlib.Path(sys.argv[2])
compiler = shutil.which(sys.argv[3])
if not compiler or not all((headers / name).is_file() for name in ('linux/io_uring.h', 'linux/futex.h')):
    print('SKIP: native ABI oracle needs host kernel UAPI headers and a C compiler')
    raise SystemExit(77)
source = pathlib.Path(__file__).with_name('native_oracle.c')
with tempfile.TemporaryDirectory(prefix='liburing-abi-') as temporary:
    directory = pathlib.Path(temporary)
    oracle = directory / 'oracle'
    # headers_install removes this kernel-internal include; it defines no futex ABI fields.
    futex = (headers / 'linux/futex.h').read_text().replace('#include <linux/compiler.h>', '')
    (directory / 'exported_futex.h').write_text(futex)
    subprocess.run([compiler, '-std=gnu11', '-D__EXPORTED_HEADERS__', '-D__user=', '-I', str(directory), '-I', str(headers), str(source), '-o', str(oracle)], check=True)
    expected = subprocess.check_output([str(oracle)], text=True)
    actual = subprocess.check_output([sys.argv[1]], text=True)
    if actual != expected:
        import difflib
        print(''.join(difflib.unified_diff(expected.splitlines(True), actual.splitlines(True))))
        raise SystemExit(1)
