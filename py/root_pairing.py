#!/usr/bin/env python3
"""Pair published root-manager APKs with the source revision used by a GKI build."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPOS = {'KSU':'tiann/KernelSU', 'KSUN':'KernelSU-Next/KernelSU-Next',
         'SKSU':'SukiSU-Ultra/SukiSU-Ultra', 'RSKSU':'ReSukiSU/ReSukiSU'}
PREFIXES = {'KSU':r'KernelSU_v[^_]+_[0-9]+-release\.apk',
            'KSUN':r'KernelSU_Next_v[^_]+_[0-9]+-release\.apk',
            'SKSU':r'SukiSU_v[^_]+_[0-9]+-release\.apk'}


def api(path):
    url = 'https://api.github.com/' + path
    token = os.environ.get('ROOT_MANAGER_TOKEN') or os.environ.get('GH_TOKEN')
    headers = {'User-Agent':'GKI-root-pairing', 'Accept':'application/vnd.github+json'}
    if token: headers['Authorization'] = 'Bearer ' + token
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        if token and e.code in (401,403):
            with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent':'GKI-root-pairing'}), timeout=30) as r:
                return json.load(r)
        raise


def download(url):
    # Public asset requests never receive the GitHub token.
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent':'GKI-root-pairing'}), timeout=45) as r:
        return r.read()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def artifact_bytes(repo, artifact_id, run_id):
    url = f'https://nightly.link/{repo}/actions/runs/{run_id}/Manager-release.zip'
    try:
        return download(url), url
    except (urllib.error.URLError, TimeoutError):
        token = os.environ.get('ROOT_MANAGER_TOKEN') or os.environ.get('GH_TOKEN')
        if not token:
            raise ValueError('Manager ReSukiSU no descargable por nightly.link; ROOT_MANAGER_TOKEN puede habilitar descarga autenticada')
        req = urllib.request.Request(f'https://api.github.com/repos/{repo}/actions/artifacts/{artifact_id}/zip',
                                     headers={'Authorization':'Bearer '+token,'User-Agent':'GKI-root-pairing'})
        try:
            with urllib.request.build_opener(NoRedirect()).open(req, timeout=30) as r:
                return r.read(), url
        except urllib.error.HTTPError as e:
            if e.code not in (301,302,303,307,308):
                raise ValueError('Descarga de artifact ReSukiSU rechazada; revisar acceso del token a Actions') from None
            location = e.headers.get('Location','')
            if not location.startswith('https://'): raise ValueError('Redirect de artifact invalido')
            return download(location), url


def apk_version(apk):
    """Read android:versionCode from binary AndroidManifest.xml, without an SDK."""
    with zipfile.ZipFile(io.BytesIO(apk)) as z: data = z.read('AndroidManifest.xml')
    strings = []
    def length(pos, utf8):
        if utf8:
            value=data[pos]; pos+=1
            if value & 0x80: value=((value & 0x7f)<<8)|data[pos]; pos+=1
        else:
            value=struct.unpack_from('<H',data,pos)[0];pos+=2
            if value & 0x8000: value=((value & 0x7fff)<<16)|struct.unpack_from('<H',data,pos)[0];pos+=2
        return value,pos
    off=struct.unpack_from('<H',data,2)[0]
    while off+8<=len(data):
        kind,header,size=struct.unpack_from('<HHI',data,off)
        if size<header or size==0 or off+size>len(data): raise ValueError('Manifest binario invalido')
        if kind==1:
            count,_,flags,start,_=struct.unpack_from('<IIIII',data,off+8)
            if count>len(data)//4: raise ValueError('String pool invalido')
            strings=[]
            for i in range(count):
                pos=off+start+struct.unpack_from('<I',data,off+header+4*i)[0]
                n,pos=length(pos,bool(flags & 0x100))
                if flags & 0x100:
                    n,pos=length(pos,True); strings.append(data[pos:pos+n].decode('utf8'))
                else: strings.append(data[pos:pos+2*n].decode('utf-16le'))
        elif kind==0x102:
            name=struct.unpack_from('<I',data,off+20)[0]
            if strings[name]=='manifest':
                start,stride,count=struct.unpack_from('<HHH',data,off+24)
                if stride<20: raise ValueError('Atributos de manifest invalidos')
                for i in range(count):
                    p=off+16+start+i*stride
                    namespace,name,raw=struct.unpack_from('<III',data,p)
                    if strings[name]=='versionCode' and namespace!=0xffffffff and strings[namespace]=='http://schemas.android.com/apk/res/android':
                        typ=data[p+15]; value=struct.unpack_from('<I',data,p+16)[0]
                        if typ not in (0x10,0x11):
                            index=raw if raw!=0xffffffff else value
                            value=int(strings[index],0)
                        if not 0<value<2**31: raise ValueError('versionCode fuera de rango')
                        return value
        off+=size
    raise ValueError('APK sin android:versionCode identificable')


def prepare(kind, ref, susfs, output, manager_dir, name):
    repo=REPOS[kind]; auto=ref=='latest'
    if not auto and kind=='KSUN' and susfs: repo='pershoot/KernelSU-Next'
    record={'kind':kind,'repo':repo,'auto':auto,'ref':ref,'susfs':susfs}
    apk=None
    if auto and kind!='RSKSU':
        release=api(f'repos/{repo}/releases/latest')
        if release.get('draft') or release.get('prerelease'): raise ValueError('Release no estable')
        ref=release['tag_name']
        assets=[a for a in release.get('assets',[]) if re.fullmatch(PREFIXES[kind],a['name']) and 'spoofed' not in a['name'].lower()]
        if len(assets)!=1: raise ValueError('Release sin un unico APK normal publicado; no se usa main')
        url=assets[0]['browser_download_url']
        if not url.startswith(f'https://github.com/{repo}/releases/download/'):
            raise ValueError('APK fuera del repositorio seleccionado')
        apk=download(url); record.update(ref=ref,manager_url=url,release_url=release['html_url'])
        sha=api(f'repos/{repo}/commits/{urllib.parse.quote(ref,safe="")}')['sha']
    elif auto:
        runs=api(f'repos/{repo}/actions/workflows/build-manager.yml/runs?branch=main&event=push&status=success&per_page=20')['workflow_runs']
        chosen=None
        for run in runs:
            if run['head_branch']!='main' or run['event']!='push' or run['conclusion']!='success': continue
            assets=api(f'repos/{repo}/actions/runs/{run["id"]}/artifacts?per_page=100')['artifacts']
            usable=[a for a in assets if a['name']=='Manager-release' and not a['expired']]
            if len(usable)==1: chosen=(run,usable[0]);break
        if not chosen: raise ValueError('No hay un build exitoso con Manager-release vigente de ReSukiSU')
        run,asset=chosen
        raw,url=artifact_bytes(repo,asset['id'],run['id'])
        with zipfile.ZipFile(io.BytesIO(raw)) as z:
            names=[n for n in z.namelist() if n.lower().endswith('.apk')]
            if len(names)!=1: raise ValueError('Artifact ReSukiSU sin un unico APK')
            apk=z.read(names[0])
        sha=run['head_sha']; record.update(ref=sha,run_id=run['id'],artifact_id=asset['id'],manager_url=url)
    else:
        sha=api(f'repos/{repo}/commits/{urllib.parse.quote(ref,safe="")}')['sha']
    if not re.fullmatch(r'[0-9a-f]{40}',sha): raise ValueError('Commit invalido')
    record['commit']=sha
    if apk is not None:
        code=apk_version(apk)
        path=Path(manager_dir)/('manager-'+re.sub(r'[^a-zA-Z0-9._-]','-',name)+'.apk')
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(apk)
        record.update(version_code=code,apk_sha256=hashlib.sha256(apk).hexdigest(),manager_file=str(path))
    Path(output).write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(record,indent=2))


def git(repo,*args):
    return subprocess.check_output(['git','-C',str(repo),*args],text=True).strip()


def install(record, kernel):
    data=json.loads(Path(record).read_text());kernel=Path(kernel).resolve();source=kernel/'KernelSU'
    if source.exists(): raise ValueError('KernelSU ya existe: usar una fuente limpia para esta compilacion')
    subprocess.run(['git','clone','--no-checkout',f'https://github.com/{data["repo"]}.git',str(source)],check=True)
    subprocess.run(['git','-C',str(source),'checkout','--detach',data['commit']],check=True)
    if git(source,'rev-parse','HEAD')!=data['commit']: raise ValueError('Checkout incorrecto')
    if not (source/'kernel/Kconfig').is_file(): raise ValueError('Estructura de kernel root no compatible')
    link=kernel/'drivers/kernelsu'
    if link.exists() or link.is_symlink(): raise ValueError('drivers/kernelsu ya existe; no se sobreescribe')
    link.symlink_to('../KernelSU/kernel')
    for name,line in [('Makefile','obj-$(CONFIG_KSU) += kernelsu/'),('Kconfig','source "drivers/kernelsu/Kconfig"')]:
        path=kernel/'drivers'/name;text=path.read_text()
        if line not in text: path.write_text(text+'\n'+line+'\n')


def pin(record, kernel):
    data=json.loads(Path(record).read_text());source=Path(kernel)/'KernelSU'
    if git(source,'rev-parse','HEAD')!=data['commit']: raise ValueError('Se cambio el commit base tras instalar root')
    if not data['auto']:
        print('Referencia explicita: no se normaliza la version ni se selecciona APK automaticamente');return
    code=data['version_code'];path=source/'kernel/Kbuild'
    if not path.is_file(): path=source/'kernel/Makefile'
    text=path.read_text()
    text,n=re.subn(r'(?m)^(\s*ccflags-y\s*\+=\s*)-DKSU_VERSION=(?:\$\([^)]+\)|[0-9]+)([ \t]*)$',
                  lambda m:m[1]+f'-DKSU_VERSION={code}'+m[2],text)
    if not n: raise ValueError('Formato de KSU_VERSION desconocido: no se oculta el error')
    path.write_text(text)
    print(f'{data["kind"]}: base {data["commit"]}, driver/manager {code}, APK SHA256 {data["apk_sha256"]}')


def main():
    parser=argparse.ArgumentParser(); sub=parser.add_subparsers(dest='op',required=True)
    p=sub.add_parser('prepare');p.add_argument('kind',choices=REPOS);p.add_argument('ref');p.add_argument('output');p.add_argument('managers');p.add_argument('name');p.add_argument('--susfs',action='store_true')
    for op in ('install','pin'):
        p=sub.add_parser(op);p.add_argument('record');p.add_argument('kernel')
    p=sub.add_parser('field');p.add_argument('record');p.add_argument('key')
    a=parser.parse_args()
    if a.op=='prepare':prepare(a.kind,a.ref,a.susfs,a.output,a.managers,a.name)
    elif a.op=='field':print(json.loads(Path(a.record).read_text())[a.key])
    elif a.op=='install':install(a.record,a.kernel)
    else:pin(a.record,a.kernel)

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,KeyError,IndexError,struct.error,zipfile.BadZipFile,subprocess.CalledProcessError) as e:
        print(f'ERROR root pairing: {e}',file=sys.stderr);sys.exit(1)
