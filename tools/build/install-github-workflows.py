#!/usr/bin/env python3
"""Install the one canonical workflow wrapper in each component repository."""
from component_recipes import load_plan
import argparse
import base64
import json
from pathlib import Path
import subprocess


def api(path, payload=None, optional=False):
    command=['gh','api',path]
    if payload is not None:
        command+=['--method','PUT','--input','-']
    result=subprocess.run(command,input=json.dumps(payload) if payload is not None else None,
                          capture_output=True,text=True)
    if result.returncode:
        if optional:return None
        raise RuntimeError(result.stderr.strip())
    return json.loads(result.stdout)


def render_workflow(plan,template):
    allowed={'ubuntu-24.04','ubuntu-24.04-arm','windows-latest'}
    if any(stage['runner'] not in allowed for stage in plan['stages'].values()):
        raise ValueError('Only the declared free standard runner labels are supported')
    for marker,runner in (('@@ARM64_STAGES@@','ubuntu-24.04-arm'),('@@WINDOWS_STAGES@@','windows-latest')):
        names=[name for name,stage in plan['stages'].items() if stage['runner']==runner]
        template=template.replace(marker,json.dumps(names,separators=(',',':')))
    return template.encode()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true')
    parser.add_argument('--repository', help='Install only this component repository wrapper')
    args=parser.parse_args()
    directory=Path(__file__).resolve().parent
    plan=load_plan()
    workflow=render_workflow(plan,(directory/'github-component.workflow.yml').read_text())
    repositories = {s['repository'] for s in plan['stages'].values()}
    if args.repository:
        if args.repository not in repositories:
            raise ValueError('Repository does not own a declared component')
        repositories = {args.repository}
    for repository in sorted(repositories):
        metadata=api('repos/'+repository)
        if metadata['private']:
            raise ValueError('Free standard runner builds require a public component repository')
        path='repos/'+repository+'/contents/.github/workflows/'+plan['workflow']
        current=api(path,optional=True)
        if current and base64.b64decode(current['content'])==workflow:
            print('OK workflow '+repository,flush=True)
            continue
        if args.check:
            raise ValueError('Missing or stale standalone component workflow: '+repository)
        payload=dict(message='ci: enable standalone ARLinux component builds [skip ci]',
                     content=base64.b64encode(workflow).decode(),branch=metadata['default_branch'])
        if current:payload['sha']=current['sha']
        result=api(path,payload)
        print('Installed '+repository+' '+result['commit']['sha'],flush=True)


if __name__=='__main__':main()
