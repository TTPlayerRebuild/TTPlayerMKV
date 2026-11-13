"""Independent plugin's pinned LGPL-2.1 Matroska/audio backend; no programs or video.

The only upstream patch exports FlagEnabled as private stream metadata. FFmpeg
otherwise ignores that flag. All patches apply to this build's private source,
never to the optional source cache supplied by the caller.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = '9.0.2'
FFMPEG_SHA = '8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e'
MAKE_SHA = 'dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3'
DECODERS = ('aac,ac3,eac3,dca,mp1float,mp2float,mp3float,flac,alac,opus,vorbis,'
            'pcm_u8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,'
            'pcm_f32le,pcm_f64le')


def download(url, path, sha):
    if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        urllib.request.urlretrieve(url, path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        raise RuntimeError('Dependency checksum mismatch: ' + str(path))


def extract(archive, directory):
    with tarfile.open(archive) as file:
        file.extractall(directory, filter='data')


def environment(vcvars, arch, directory):
    script = directory / ('environment-' + arch + '.cmd')
    script.write_text('@echo off\ncall "' + str(vcvars) + '" ' + arch + ' >nul\nset\n', encoding='utf-8')
    output = subprocess.run(['cmd', '/d', '/c', str(script)], capture_output=True, check=True)
    env = {k.upper(): v for k, v in os.environ.items()}
    for line in output.stdout.decode('mbcs').splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key.upper()] = value
    return env


def run(command, cwd, env, log):
    with log.open('w', encoding='utf-8') as file:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=file, stderr=subprocess.STDOUT)
    if result.returncode:
        print('\n'.join(log.read_text(encoding='utf-8', errors='replace').splitlines()[-70:]))
        raise RuntimeError('Backend build failed; see ' + str(log))


def patch_source(source):
    path = source / 'libavformat/matroskadec.c'
    text = path.read_text(encoding='utf-8')
    if 'ttplayer_track_enabled' in text:
        return
    replacements = [
        ('    uint64_t default_duration;\n    uint64_t flag_default;',
         '    uint64_t default_duration;\n    uint64_t flag_enabled;\n    uint64_t flag_default;'),
        ('{ MATROSKA_ID_TRACKFLAGENABLED,      EBML_NONE }',
         '{ MATROSKA_ID_TRACKFLAGENABLED,      EBML_UINT, 0, 0, offsetof(MatroskaTrack, flag_enabled), { .u = 1 } }'),
        ('        par->codec_id  = codec_id;\n\n        if (track->flag_default)',
         '        par->codec_id  = codec_id;\n'
         '        av_dict_set(&st->metadata, "ttplayer_track_enabled", track->flag_enabled ? "1" : "0", 0);\n\n'
         '        if (track->flag_default)'),
    ]
    for old, new in replacements:
        if text.count(old) != 1:
            raise RuntimeError('Pinned Matroska FlagEnabled patch no longer applies')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--vs', type=Path, required=True)
    parser.add_argument('--ltl', type=Path, required=True)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--bash', type=Path)
    parser.add_argument('--make-archive', type=Path)
    args = parser.parse_args()
    args.vs = args.vs.resolve()
    args.ltl = args.ltl.resolve()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    git = shutil.which('git')
    bash = args.bash or (Path(git).resolve().parent.parent / 'bin/bash.exe' if git else None)
    if not bash or not bash.is_file():
        raise RuntimeError('Git for Windows bash.exe is required; set TTP_MKV_BASH')
    source = build / ('ffmpeg-' + VERSION)
    if not source.exists():
        if args.source:
            if args.source.resolve() == source:
                raise RuntimeError('External source must not be the private patched source')
            shutil.copytree(args.source, source, ignore=shutil.ignore_patterns('.git'))
        else:
            archive = build / ('ffmpeg-' + VERSION + '.tar.xz')
            download('https://ffmpeg.org/releases/' + archive.name, archive, FFMPEG_SHA)
            extract(archive, build)
    if (source / 'RELEASE').read_text().strip() != VERSION:
        raise RuntimeError('Expected FFmpeg ' + VERSION)
    patch_source(source)
    vcvars = args.vs / 'VC/Auxiliary/Build/vcvarsall.bat'
    make_source = build / 'make-4.4.1'
    make = make_source / 'WinRel/gnumake.exe'
    if not make.is_file():
        if not make_source.exists():
            archive = build / 'make-4.4.1.tar.gz'
            if args.make_archive and args.make_archive.resolve() != archive:
                shutil.copyfile(args.make_archive, archive)
            download('https://ftp.gnu.org/gnu/make/' + archive.name, archive, MAKE_SHA)
            extract(archive, build)
        run(['cmd', '/d', '/c', 'build_w32.bat', '--without-guile'], make_source,
            environment(vcvars, 'x64', build), build / 'make-build.log')
    out = build / 'static'
    out.mkdir(exist_ok=True)
    options = ['--toolchain=msvc', '--target-os=win32', '--arch=x86', '--disable-asm',
               '--disable-everything', '--disable-autodetect', '--disable-programs', '--disable-doc',
               '--disable-network', '--disable-avdevice', '--disable-avfilter', '--disable-swscale',
               '--enable-swresample', '--enable-w32threads', '--disable-pthreads', '--disable-debug',
               '--disable-iconv', '--enable-small', '--enable-static', '--disable-shared',
               '--enable-demuxer=matroska', '--enable-decoder=' + DECODERS,
               '--enable-parser=aac,ac3,dca,flac,mpegaudio,opus,vorbis',
               '--extra-cflags=-MT -O1 -Gy -Gw -D_WIN32_WINNT=0x0601',
               '--extra-ldflags=-NODEFAULTLIB:libcmt -DEFAULTLIB:msvcrt']
    digest = hashlib.sha256()
    for path in sorted(source.rglob('*')):
        if path.is_file() and (path.suffix in ('.c', '.h', '.S', '.asm', '.mak') or path.name in ('configure', 'Makefile')):
            digest.update(path.relative_to(source).as_posix().encode())
            digest.update(path.read_bytes())
    identity = dict(version=VERSION, archive_sha256=FFMPEG_SHA, source_sha256=digest.hexdigest(),
                    script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    options=options, vs=str(args.vs), ltl=str(args.ltl))
    stamp = out / 'build-identity.json'
    libraries = [out / ('lib' + lib) / (lib + '.lib') for lib in ('avformat', 'avcodec', 'swresample', 'avutil')]
    if stamp.exists() and json.loads(stamp.read_text()) == identity and all(p.exists() for p in libraries):
        return
    env = environment(vcvars, 'x86', build)
    env['INCLUDE'] = str(args.ltl / 'TargetPlatform/5.1.2600.0/header') + ';' + str(args.ltl / 'TargetPlatform/header') + ';' + env.get('INCLUDE', '')
    env['LIB'] = str(args.ltl / 'TargetPlatform/5.1.2600.0/lib/Win32') + ';' + env.get('LIB', '')
    env['MSYS2_ARG_CONV_EXCL'] = '*'
    env['PATH'] = str(bash.parent.parent / 'usr/bin') + ';' + str(bash.parent) + ';' + env['PATH']
    (out / 'Makefile').unlink(missing_ok=True)
    configure = ' '.join(shlex.quote(x) for x in ['bash', (source / 'configure').as_posix()] + options)
    run([str(bash), '-c', configure], out, env, build / 'configure.log')
    config = (out / 'config.h').read_text(encoding='utf-8')
    for forbidden in ('CONFIG_GPL', 'CONFIG_NONFREE', 'CONFIG_VERSION3', 'CONFIG_GPLV3'):
        if '#define ' + forbidden + ' 0' not in config:
            raise RuntimeError('Backend must remain LGPL-2.1: ' + forbidden)
    components = (out / 'config_components.h').read_text(encoding='utf-8')
    for decoder in DECODERS.split(','):
        if '#define CONFIG_' + decoder.upper() + '_DECODER 1' not in components:
            raise RuntimeError('Required audio decoder was disabled by configure: ' + decoder)
    for path in (out / 'Makefile', out / 'ffbuild/config.mak'):
        text = path.read_text(encoding='utf-8')
        text = re.sub(r'''(^|[ \t="'])/([a-zA-Z])/''', lambda m: m[1] + m[2].upper() + ':/', text, flags=re.MULTILINE)
        path.write_text(text, encoding='utf-8')
    shell = 'SHELL=' + (bash.parent.parent / 'usr/bin/sh.exe').as_posix()
    run([str(make), 'clean', shell], out, env, build / 'clean.log')
    run([str(make), '-j4', 'CCDEP=', 'CXXDEP=', 'HOSTCCDEP=', shell], out, env, build / 'build.log')
    if not all(p.exists() for p in libraries):
        raise RuntimeError('Matroska static libraries missing')
    stamp.write_text(json.dumps(identity, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
