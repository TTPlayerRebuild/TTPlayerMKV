"""Build a complete corresponding-source attachment, excluding private tests."""
import argparse
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--version', required=True)
    args = parser.parse_args()
    root, build = args.root.resolve(), args.build.resolve()
    backend = Path((build / 'backend-path.txt').read_text())
    output = build / 'Release' / ('ttp_mkv-' + args.version + '-source.zip')
    excluded = {'.git', 'build', 'artifact', '__pycache__'}
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(root.rglob('*')):
            relative = path.relative_to(root)
            if path.is_file() and not any(part in excluded or part.startswith('build-') for part in relative.parts):
                archive.write(path, 'ttp_mkv/' + relative.as_posix())
        for path in sorted((backend / 'ffmpeg-9.0.2').rglob('*')):
            if path.is_file() and '.git' not in path.parts:
                archive.write(path, 'ttp_mkv/third_party/ffmpeg/' + path.relative_to(backend / 'ffmpeg-9.0.2').as_posix())
        archive.write(backend / 'make-4.4.1.tar.gz', 'ttp_mkv/third_party/make-4.4.1.tar.gz')
        archive.write(backend / 'static/build-identity.json', 'ttp_mkv/third_party/backend-build-identity.json')
        archive.writestr('ttp_mkv/BUILD_VERSION', args.version + '\n')
    print(output)


if __name__ == '__main__':
    main()
