#!/usr/bin/env python3
"""Dispatch independent repository workflows from Windows or Linux, without WSL."""
from component_recipes import load_plan
import argparse
from contextlib import contextmanager
import errno
import os
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import tarfile
import time
from threading import Event
from artifact_download import download,sha256_file,reuse_product,download_capsule
from component_inputs import request_identity, source_inputs


def gh(*args, optional=False):
    attempts=4 if args[0] in ('api','run') else 1
    for attempt in range(attempts):
        result = subprocess.run(['gh', *map(str,args)], capture_output=True, text=True)
        if not result.returncode or not any(message in result.stderr.lower() for message in
                ('timeout','timed out','connection','eof','502','503','504','tls handshake')):
            break
        if attempt+1<attempts:
            time.sleep(2**attempt)
    if result.returncode:
        if optional:
            return None
        raise RuntimeError(result.stderr.strip())
    return result.stdout


def api(path, optional=False):
    value = gh('api', path, optional=optional)
    return json.loads(value) if value else None


@contextmanager
def request_lock(path, stopped):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a+b') as lock:
        if not path.stat().st_size:
            lock.write(b'0')
            lock.flush()
        deadline = time.monotonic() + 8*60*60
        while True:
            try:
                lock.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError as error:
                if error.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                    raise
                if stopped.wait(0.25):
                    raise RuntimeError('Local coordinator stopped while waiting for a component')
                if time.monotonic() > deadline:
                    raise TimeoutError('Timed out waiting for component request lock')
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == 'nt':
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


class Coordinator:
    def __init__(self, plan, framework, android, state, poll=30):
        self.plan,self.framework,self.android,self.state,self.poll = plan,framework,android,state,poll
        state.mkdir(parents=True,exist_ok=True)
        self.stopped = Event()

    def runs(self, repository):
        return json.loads(gh('run','list','--repo',repository,'--workflow',self.plan['workflow'],
            '--limit','100','--json','databaseId,displayTitle,status,conclusion,url'))

    def existing(self, request):
        repository = self.plan['stages'][request['stage']]['repository']
        tag = self.plan['stages'][request['stage']]['release']
        release = api(f'repos/{repository}/releases/tags/{tag}',optional=True)
        if not release:
            return None
        asset = next((a for a in release['assets']
                      if a['name']=='component-'+request['identity']+'.json'),None)
        if not asset:
            return None
        digest = asset.get('digest','')
        if not digest.startswith('sha256:'):
            raise ValueError('GitHub did not supply the component manifest SHA-256')
        manifest = self.state/request['identity']/'component.json'
        download(asset['browser_download_url'],manifest,digest[7:])
        record = json.loads(manifest.read_text())
        for name in ('stage','identity','android','dependencies','sourceInputs'):
            if record.get(name) != request.get(name):
                return None
        names={a['name'] for a in release['assets']}
        if any(part['name'] not in names for part in record['parts']):
            return None
        return dict(repository=repository,identity=request['identity'],
            manifestUrl=asset['browser_download_url'],manifestSha256=digest[7:])

    def component(self, stage, dependencies):
        framework = Path(__file__).resolve().parents[2]
        request = dict(format=2,stage=stage,framework=self.framework,android=self.android,
                       dependencies=dependencies,sourceInputs=source_inputs(framework,self.framework,stage,self.plan))
        request['identity']=request_identity(request)
        with request_lock(self.state/request['identity']/'dispatch.lock', self.stopped):
            return self._component(request)

    def _component(self, request):
        stage = request['stage']
        repository=self.plan['stages'][stage]['repository']
        result=self.existing(request)
        if result:
            print('OK published '+repository+'/'+stage,flush=True)
            return result
        payload=json.dumps(request,sort_keys=True)
        # A standalone workflow_dispatch creates a run in the component repo.
        # Do not replace this with workflow_call: caches must belong to that repo.
        title='arlinux/'+stage+'/'+request['identity']
        previous_runs=self.runs(repository)
        run=next((r for r in previous_runs if r['displayTitle']==title and r['status']!='completed'),None)
        ignored={r['databaseId'] for r in previous_runs if r['status']=='completed'}
        if not run:
            # Capsules contain verified build receipts, including references.
            # They can seed a new run after an unrelated framework commit; the
            # producer's input hashes, not this coordinator, decide cache hits.
            if stage not in ('yibu','debian','arch','omarchy','plasma','lxqt','windows-fonts','wine-product'):
                candidate=api(f"repos/{repository}/releases/tags/{self.plan['stages'][stage]['release']}", optional=True)
                if candidate:
                    asset=next((a for a in candidate['assets'] if a['name']=='component.json'), None)
                    digest=asset.get('digest','') if asset else ''
                    if digest.startswith('sha256:'):
                        request['cacheSeed']=dict(repository=repository,
                            identity='',
                            manifestUrl=asset['browser_download_url'],manifestSha256=digest[7:])
                        # Read the actual identity from the verified fixed-release metadata.
                        file=self.state/request['identity']/'seed.json'
                        download(asset['browser_download_url'],file,digest[7:])
                        seed=json.loads(file.read_text())
                        if seed['stage']!=stage:
                            raise ValueError('Cache seed belongs to a different component')
                        request['cacheSeed']['identity']=seed['identity']
                        fixed=next((a for a in candidate['assets']
                                    if a['name']=='component-'+seed['identity']+'.json'),None)
                        if fixed:
                            request['cacheSeed']['manifestUrl']=fixed['browser_download_url']
            payload=json.dumps(request,sort_keys=True)
            gh('workflow','run',self.plan['workflow'],'--repo',repository,'-f','request='+payload)
            print('START '+repository+'/'+stage,flush=True)
        else:
            print('RESUME '+repository+'/'+stage+' '+run['url'],flush=True)
        deadline=time.monotonic()+7*60*60
        previous=None
        while time.monotonic()<deadline:
            if self.stopped.is_set():
                raise RuntimeError('Local coordinator stopped; remote workflows remain available to resume')
            runs=self.runs(repository)
            matches=[r for r in runs if r['displayTitle']==title and r['databaseId'] not in ignored]
            run=next((r for r in matches if r['conclusion']=='success'),
                     next((r for r in matches if r['status']!='completed'), matches[0] if matches else None))
            if run:
                status=run['status']+'/'+str(run['conclusion'])
                if status!=previous:
                    print(stage+': '+status+' '+run['url'],flush=True)
                    previous=status
                if run['status']=='completed':
                    if run['conclusion']!='success':
                        raise RuntimeError(stage+' failed: '+run['url'])
                    result=self.existing(request)
                    if not result:
                        raise ValueError('Successful workflow did not publish its verified capsule')
                    return result
            self.stopped.wait(self.poll)
        raise TimeoutError('Component workflow timed out: '+stage)

    def run(self, products, parallel):
        required=set()
        def visit(stage):
            if stage in required:
                return
            for dependency in self.plan['stages'][stage]['dependencies']:
                visit(dependency)
            required.add(stage)
        for product in products:
            visit(self.plan['products'][product])
        completed={}
        pending=set(required)
        active={}
        with ThreadPoolExecutor(max_workers=parallel) as executor:
            while pending or active:
                for stage in sorted(pending):
                    dependencies=self.plan['stages'][stage]['dependencies']
                    if all(name in completed for name in dependencies) and len(active)<parallel:
                        future=executor.submit(self.component,stage,[completed[n] for n in dependencies])
                        active[future]=stage
                        pending.remove(stage)
                if not active:
                    raise ValueError('Component dependency graph contains a cycle')
                done,_=wait(active,return_when=FIRST_COMPLETED)
                for future in done:
                    stage=active.pop(future)
                    try:
                        completed[stage]=future.result()
                    except BaseException:
                        self.stopped.set()
                        raise
                    (self.state/'progress.json').write_text(json.dumps(completed,indent=2)+'\n')
        return {product:completed[self.plan['products'][product]] for product in products}


def fetch_product(reference, output):
    identity = reference['identity']
    if not re.fullmatch('[0-9a-f]{64}', identity):
        raise ValueError('Invalid product component identity')
    directory = output.parent/'github-state/downloads'/identity
    with request_lock(directory/'transfer.lock', Event()):
        if reuse_product(output, reference):
            return
        cache_receipt=output/'download-receipt.json'
        import zstandard
        manifest=directory/'component.json'
        download(reference['manifestUrl'],manifest,reference['manifestSha256'])
        record=json.loads(manifest.read_text())
        if record['identity']!=reference['identity']:
            raise ValueError('Product identity mismatch')
        archive=download_capsule(record, directory)
        tools = record['stage'] == 'guest-tools'
        product='guest-tools' if tools else 'capture' if record['stage']=='vulkan-capture' else 'rootfs/'+record['stage']
        prefix='framework/build/artifacts/'+product+'/'
        output.mkdir(parents=True,exist_ok=True)
        with archive.open('rb') as source,zstandard.ZstdDecompressor().stream_reader(source) as decoded:
            with tarfile.open(fileobj=decoded,mode='r|') as capsule:
                for member in capsule:
                    if not member.isfile():
                        continue
                    if not member.name.startswith(prefix):
                        continue
                    name=member.name[len(prefix):]
                    if '\\' in name or ':' in name or any(p in ('','.','..') for p in name.split('/')) or '/' in name:
                        raise ValueError('Unsafe product archive member')
                    (output/name).parent.mkdir(parents=True,exist_ok=True)
                    with (output/name).open('wb') as dest:
                        import shutil
                        shutil.copyfileobj(capsule.extractfile(member),dest)
        (output/'github-component.json').write_text(json.dumps(record,indent=2)+'\n')
        capture = record['stage']=='vulkan-capture'
        basename = 'guest-tools' if tools else 'capture' if capture else 'default'
        receipt=json.loads((output/(basename+'.json')).read_text())
        with (output/(basename+'.zip')).open('rb') as source:
            digest=hashlib.file_digest(source,'sha256').hexdigest()
        if digest!=receipt['archiveSha256' if capture or tools else 'desktopSha256']:
            raise ValueError('Build receipt does not match the downloaded product')
        files={p.relative_to(output).as_posix():sha256_file(p)
               for p in output.rglob('*') if p.is_file() and p!=cache_receipt}
        cache_receipt.write_text(json.dumps(dict(format=1,reference=reference,files=files),indent=2)+'\n')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework-ref',default='main')
    parser.add_argument('--distribution', choices=('yibu','debian','arch','omarchy','plasma','lxqt'), default='yibu')
    parser.add_argument('--product',choices=('rootfs','guest-tools','capture','all'),default='all')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--parallel',type=int,default=8)
    parser.add_argument('--plan',action='store_true')
    parser.add_argument('--metadata-only',action='store_true',
                        help='Resolve/build the same graph, leaving payload downloads to the device')
    args=parser.parse_args()
    framework=Path(__file__).resolve().parents[2]
    plan=load_plan()
    if args.plan:
        print(json.dumps(plan,indent=2))
        return
    framework_sha=(args.framework_ref if re.fullmatch('[0-9a-f]{40}',args.framework_ref)
                   else api('repos/taowen/arlinux-rootfs/commits/'+args.framework_ref)['sha'])
    products=['rootfs'] if args.product=='all' else [args.product]
    if 'rootfs' in products:
        products.append('guest-tools')
    if args.distribution != 'yibu':
        plan['products']['rootfs'] = args.distribution
    state=args.output/'github-state'
    coordinator=Coordinator(plan,framework_sha,None,state)
    result=coordinator.run(products,args.parallel)
    if not args.metadata_only:
        for name,reference in result.items():
            fetch_product(reference,args.output/name)
    (args.output/'products.json').write_text(json.dumps(dict(format=1,
        framework=framework_sha,products=result),indent=2)+'\n')


if __name__=='__main__':
    main()
