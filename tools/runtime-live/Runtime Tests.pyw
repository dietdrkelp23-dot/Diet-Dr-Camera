"""Private OmniCam runtime test launcher. Run with pythonw.exe."""
from pathlib import Path
import argparse, ctypes, datetime, hashlib, json, os, queue, re, shutil, subprocess, sys, threading, time
from ctypes import wintypes
ROOT = Path(__file__).resolve().parent
MANIFESTS = ROOT / 'manifests'
ACTIVE = ROOT / 'active-session.json'
# Stable on-disk identity preserves existing saves, profiles and run histories.
DEFAULT_PROFILE = 'DDC 1.2.1'

def read_json(p):
    return json.loads(Path(p).read_text(encoding='utf-8-sig'))

def write_json(p, data):
    p = Path(p); p.parent.mkdir(parents=True, exist_ok=True)
    temp = p.with_suffix(p.suffix + '.tmp')
    temp.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    temp.replace(p)

def sha256(p):
    with Path(p).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def known_folder(csidl):
    buf = ctypes.create_unicode_buffer(32768)
    if ctypes.windll.shell32.SHGetFolderPathW(None, csidl, None, 0, buf) != 0:
        raise RuntimeError('Cannot locate Windows known folder')
    return Path(buf.value)

def hidden_options():
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = subprocess.SW_HIDE
    return dict(shell=False, creationflags=subprocess.CREATE_NO_WINDOW, startupinfo=si)

def child_environment():
    env = os.environ.copy()
    defaults = {'SystemDrive':'C:', 'SystemRoot':r'C:\Windows', 'windir':r'C:\Windows',
                'ProgramData':r'C:\ProgramData', 'ALLUSERSPROFILE':r'C:\ProgramData',
                'ProgramFiles':r'C:\Program Files', 'ProgramFiles(x86)':r'C:\Program Files (x86)',
                'ProgramW6432':r'C:\Program Files', 'ComSpec':r'C:\Windows\System32\cmd.exe',
                'PATHEXT':'.COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC'}
    defaults.update(APPDATA=str(known_folder(26)), LOCALAPPDATA=str(known_folder(28)))
    for k,v in defaults.items(): env.setdefault(k,v)
    return env

def process_names():
    class ENTRY(ctypes.Structure):
        _fields_ = [('dwSize',wintypes.DWORD),('cntUsage',wintypes.DWORD),('th32ProcessID',wintypes.DWORD),
                    ('th32DefaultHeapID',ctypes.c_size_t),('th32ModuleID',wintypes.DWORD),
                    ('cntThreads',wintypes.DWORD),('th32ParentProcessID',wintypes.DWORD),
                    ('pcPriClassBase',wintypes.LONG),('dwFlags',wintypes.DWORD),('szExeFile',wintypes.WCHAR*260)]
    k = ctypes.WinDLL('kernel32',use_last_error=True)
    k.CreateToolhelp32Snapshot.argtypes=[wintypes.DWORD,wintypes.DWORD]; k.CreateToolhelp32Snapshot.restype=wintypes.HANDLE
    k.Process32FirstW.argtypes=[wintypes.HANDLE,ctypes.POINTER(ENTRY)]
    k.Process32NextW.argtypes=[wintypes.HANDLE,ctypes.POINTER(ENTRY)]
    k.CloseHandle.argtypes=[wintypes.HANDLE]
    h=k.CreateToolhelp32Snapshot(2,0)
    if h == wintypes.HANDLE(-1).value: raise ctypes.WinError(ctypes.get_last_error())
    names=[]
    try:
        e=ENTRY();e.dwSize=ctypes.sizeof(e)
        ok=k.Process32FirstW(h,ctypes.byref(e))
        while ok:
            names.append((e.szExeFile.lower(),e.th32ProcessID))
            ok=k.Process32NextW(h,ctypes.byref(e))
    finally:k.CloseHandle(h)
    return names

class SessionLock:
    """Serialize recovery and launching across every copy of this launcher."""
    def __enter__(self):
        self.k=ctypes.WinDLL('kernel32',use_last_error=True)
        self.k.CreateMutexW.argtypes=[ctypes.c_void_p,wintypes.BOOL,wintypes.LPCWSTR]
        self.k.CreateMutexW.restype=wintypes.HANDLE
        self.k.WaitForSingleObject.argtypes=[wintypes.HANDLE,wintypes.DWORD]
        self.k.WaitForSingleObject.restype=wintypes.DWORD
        self.k.ReleaseMutex.argtypes=[wintypes.HANDLE]
        self.k.CloseHandle.argtypes=[wintypes.HANDLE]
        self.handle=self.k.CreateMutexW(None,False,'Local\\DietDrCameraRuntimeTests1_2_1')
        if not self.handle:raise ctypes.WinError(ctypes.get_last_error())
        result=self.k.WaitForSingleObject(self.handle,0)
        if result not in (0,0x80):
            self.k.CloseHandle(self.handle)
            if result==258:raise RuntimeError('Another runtime test is already starting or running. Finish it first.')
            raise ctypes.WinError(ctypes.get_last_error())
        return self
    def __exit__(self,*exc):
        self.k.ReleaseMutex(self.handle);self.k.CloseHandle(self.handle)

def ensure_idle():
    conflicts=[(name,pid) for name,pid in process_names() if name in ('skyrimse.exe','skse64_loader.exe','modorganizer.exe')]
    if conflicts:
        raise RuntimeError('Close Skyrim and Mod Organizer before starting a test. Running: '+', '.join(n for n,p in conflicts))

def inside(p,root=ROOT):
    p=Path(p).resolve();root=Path(root).resolve()
    if not p.is_relative_to(root):raise RuntimeError('Path outside test installation: '+str(p))
    return p

def load_config(runtime):
    if not re.fullmatch(r'1\.[567]\.\d+',runtime):raise ValueError('Invalid runtime')
    return read_json(MANIFESTS/('setup-'+runtime+'.json'))

def prepared_runtimes():
    targets=MANIFESTS/'targets.json'
    excluded={t['runtime'] for t in read_json(targets) if t.get('support')=='excluded'} if targets.is_file() else set()
    versions=[p.stem.removeprefix('setup-') for p in MANIFESTS.glob('setup-1.*.json')]
    return sorted((v for v in versions if v not in excluded),key=lambda v:tuple(int(n) for n in v.split('.')))

def profile_choices(cfg):
    return {p.get('label',p['name']):p['name'] for p in cfg['profiles']}

def check_manager_paths(settings,manager,game):
    def setting(key):
        match=re.search(r'^'+re.escape(key)+r'=(.*)$',settings,re.M)
        return match.group(1).strip() if match else None
    def expect_path(key,wanted):
        value=setting(key)
        if value is None or Path(value.replace('\\\\','\\')).resolve()!=wanted.resolve():
            raise RuntimeError('Manager path changed: '+key)
    for key,folder in [('base_directory',''),('mod_directory','mods'),('profiles_directory','profiles'),('overwrite_directory','overwrite'),('download_directory','downloads')]:
        expect_path(key,manager/folder)
    matches=re.findall(r'^(\d+)\\title=SKSE\s*$',settings,re.M)
    if len(matches)!=1:raise RuntimeError('Expected exactly one SKSE launch entry')
    prefix=matches[0]+'\\'
    expect_path(prefix+'binary',game/'skse64_loader.exe')
    expect_path(prefix+'workingDirectory',game)
    if setting(prefix+'arguments') not in ('',None):raise RuntimeError('SKSE launch arguments changed')

def profile_lines(path):
    return [s.strip() for s in Path(path).read_text(encoding='utf-8-sig').splitlines() if s.strip() and not s.lstrip().startswith('#')]

def first_run_preferences(runtime):
    version=tuple(int(part) for part in runtime.split('.'))
    if version < (1,6,0):return {}
    flags={'bFreebiesSeen':'1'}
    if version >= (1,6,1130):flags['bUpsellOwned']='1'
    return flags

def prepare_preferences(runtime, path, repair=False):
    """Skip the Anniversary first-run download prompt in this test profile only."""
    flags=first_run_preferences(runtime)
    if not flags:return False
    path=Path(path);data=path.read_bytes();text=data.decode('utf-8-sig')
    lines=text.splitlines(keepends=True);section='';observed={key.lower():[] for key in flags}
    for line in lines:
        header=re.match(r'^\s*\[([^\]]+)\]',line)
        if header:section=header.group(1).strip().lower()
        elif section=='general' and '=' in line:
            key,value=line.split('=',1);key=key.strip().lower()
            if key in observed:observed[key].append(re.split(r'[;#]',value,maxsplit=1)[0].strip())
    if all(values==['1'] for values in observed.values()):return False
    if not repair:raise RuntimeError('Anniversary prompt settings need repair in '+str(path)+'. Launch test to repair them automatically.')
    newline='\r\n' if '\r\n' in text else '\n'
    entries=''.join(key+'='+value+newline for key,value in flags.items())
    result=[];section='';inserted=False
    for line in lines:
        header=re.match(r'^\s*\[([^\]]+)\]',line)
        if header:
            section=header.group(1).strip().lower();result.append(line)
            if section=='general' and not inserted:
                if not line.endswith(('\r','\n')):result.append(newline)
                result.append(entries);inserted=True
        elif section=='general' and '=' in line and line.split('=',1)[0].strip().lower() in observed:continue
        else:result.append(line)
    if not inserted:
        if text and not text.endswith(('\r','\n')):result.append(newline)
        result.extend(['[General]'+newline,entries])
    bom=b'\xef\xbb\xbf' if data.startswith(b'\xef\xbb\xbf') else b''
    temp=path.with_name(path.name+'.tmp')
    temp.write_bytes(bom+''.join(result).encode('utf-8'));temp.replace(path)
    return True

def check_setup(runtime, profile=DEFAULT_PROFILE, full=False, prepare=False):
    cfg=load_config(runtime)
    game=inside(cfg['game']);manager=inside(cfg['manager'])
    spec=next((p for p in cfg['profiles'] if profile in (p['name'],p.get('label'))),None)
    if spec is None:raise RuntimeError('Profile is not configured for this runtime')
    profile=spec['name']
    if not (manager/'portable.txt').is_file():raise RuntimeError('Portable manager marker missing')
    settings=(manager/'ModOrganizer.ini').read_text(encoding='utf-8-sig')
    match=re.search(r'^gamePath=@ByteArray\((.*)\)$',settings,re.M)
    actual=Path(match.group(1).strip().replace('\\\\','\\')).resolve() if match else None
    if actual!=game:raise RuntimeError('Manager game directory changed')
    check_manager_paths(settings,manager,game)
    pro=manager/'profiles'/profile
    ini=(pro/'settings.ini').read_text(encoding='utf-8-sig')
    for line in ('LocalSaves=true','LocalSettings=true','SKSE='+spec['output']):
        if line not in ini.splitlines():raise RuntimeError('Profile isolation setting changed: '+line)
    actual_mods=[s.strip() for s in (pro/'modlist.txt').read_text(encoding='utf-8-sig').splitlines() if s.startswith('+')]
    if actual_mods!=spec['enabledLines']:raise RuntimeError('Enabled mods or priority differ from the recorded setup')
    for fname in ('plugins.txt','loadorder.txt'):
        expected=spec.get('profileLines',{}).get(fname)
        if expected is not None:
            if profile_lines(pro/fname)!=expected:raise RuntimeError('Plugin list changed: '+fname)
        elif sha256(pro/fname)!=spec['profileHashes'][fname]:raise RuntimeError('Plugin list changed: '+fname)
    for record in cfg['protectedFiles']:
        p=inside(record['path'])
        if not p.is_file() or p.stat().st_size!=record['bytes']:raise RuntimeError('File missing/size changed: '+str(p))
        if full or record.get('alwaysHash',True):
            if sha256(p)!=record['sha256']:raise RuntimeError('File hash changed: '+str(p))
    for record in cfg['gameFiles']:
        p=inside(game/record['path'])
        if not p.is_file() or p.stat().st_size!=record['bytes']:raise RuntimeError('Game file missing/size changed: '+str(p))
        if full or p.suffix.lower() in ('.exe','.dll'):
            if sha256(p)!=record['sha256']:raise RuntimeError('Game file hash changed: '+str(p))
    prepare_preferences(runtime,pro/'SkyrimPrefs.ini',repair=prepare)
    return cfg,spec

class Isolation:
    def __init__(self,run,runtime,documents=None,local=None):
        self.run=Path(run);self.runtime=runtime
        self.logroot=(Path(documents) if documents else known_folder(5))/'My Games'/'Skyrim Special Edition'/'SKSE'
        self.catalog=(Path(local) if local else known_folder(28))/'Skyrim Special Edition'/'ContentCatalog.txt'
        self.cache=ROOT/'catalogs'/runtime/'ContentCatalog.txt'
        self.record=self.run/'isolation.json'
    def begin(self):
        files=[]
        for kind,paths in [('catalog',[self.catalog]),('logs',list(self.logroot.glob('*.log*')) if self.logroot.exists() else [])]:
            for p in paths:
                b=self.run/'before-shared'/kind/p.name
                item=dict(path=str(p),backup=str(b),existed=p.is_file(),sha256=None,kind=kind)
                if p.is_file():
                    b.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,b)
                    item['sha256']=sha256(p)
                    if sha256(b)!=item['sha256']:raise RuntimeError('Isolation backup failed')
                files.append(item)
        self.state=dict(status='backed-up',runtime=self.runtime,files=files,logroot=str(self.logroot),catalog=str(self.catalog),cache=str(self.cache))
        write_json(self.record,self.state)
        self.state['status']='active';write_json(self.record,self.state)
        # Start with fresh logs so a failed launch cannot inherit an old pass.
        for item in files:
            if item['kind']=='logs' and item['existed']:Path(item['path']).unlink()
        self.catalog.parent.mkdir(parents=True,exist_ok=True)
        if self.cache.is_file():shutil.copy2(self.cache,self.catalog)
        else:self.catalog.unlink(missing_ok=True)
    def restore(self):
        restore_isolation(self.record)

def restore_isolation(record):
    record=Path(record);state=read_json(record)
    if state['status']=='restored':return
    run=record.parent;logroot=Path(state['logroot']);catalog=Path(state['catalog'])
    # Validate every backup before touching the shared originals.
    for item in state['files']:
        if item['existed'] and sha256(item['backup'])!=item['sha256']:
            raise RuntimeError('A recovery backup changed; retained at '+item['backup'])
    after=run/'after-shared';after.mkdir(exist_ok=True)
    if catalog.is_file():
        shutil.copy2(catalog,after/'ContentCatalog.txt')
        cache=Path(state['cache']);cache.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(catalog,cache)
    original_logs={Path(r['path']).name for r in state['files'] if r['kind']=='logs'}
    for p in logroot.glob('*.log*') if logroot.exists() else []:
        dest=after/'SKSE'/p.name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        if p.name not in original_logs:p.unlink()
    for item in state['files']:
        p=Path(item['path'])
        if item['existed']:
            p.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(item['backup'],p)
            if sha256(p)!=item['sha256']:raise RuntimeError('Restoration hash mismatch: '+str(p))
        else:p.unlink(missing_ok=True)
    state['status']='restored';state['restoredAt']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    write_json(record,state)

def _recover_interrupted_locked():
    if not ACTIVE.exists():return None
    ensure_idle();active=read_json(ACTIVE);run=inside(active['run'])
    if (run/'isolation.json').exists():restore_isolation(run/'isolation.json')
    active['recoveredAt']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    write_json(run/'recovered-session.json',active);ACTIVE.unlink()
    return str(run)

def recover_interrupted():
    with SessionLock():return _recover_interrupted_locked()

def run_session(runtime,profile,notify=lambda text:None):
    with SessionLock():return _run_session_locked(runtime,profile,notify)

def _run_session_locked(runtime,profile,notify=lambda text:None):
    ensure_idle();recover_interrupted()
    if not any(n=='steam.exe' for n,p in process_names()):raise RuntimeError('Start Steam, then launch this test again')
    notify('Checking the selected files and profile...')
    cfg,spec=check_setup(runtime,profile,prepare=True)
    profile=spec['name']
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    run=ROOT/'runs'/(runtime+'-'+profile.replace(' ','_')+'-'+stamp);run.mkdir(parents=True)
    record=dict(runtime=runtime,profile=profile,profileLabel=spec.get('label',profile),ddcDllSha256=spec.get('ddcHash'),startedAt=datetime.datetime.now(datetime.timezone.utc).isoformat(),startup='unreviewed',gameplay='not-run',notes='',error=None)
    write_json(run/'run.json',record)
    for name in ('modlist.txt','plugins.txt','loadorder.txt','settings.ini','Skyrim.ini','SkyrimPrefs.ini','SkyrimCustom.ini'):
        p=Path(cfg['manager'])/'profiles'/profile/name
        if p.is_file():shutil.copy2(p,run/name)
    output_before=Path(cfg['manager'])/'mods'/spec['output']
    if output_before.is_dir():shutil.copytree(output_before,run/'settings-before')
    shutil.copy2(MANIFESTS/('setup-'+runtime+'.json'),run/'setup.json')
    (run/'CHECKLIST.md').write_text((ROOT/'CHECKLIST.md').read_text(encoding='utf-8'),encoding='utf-8')
    with ACTIVE.open('x',encoding='utf-8') as f:json.dump(dict(run=str(run),launcherPid=os.getpid()),f)
    isolation=Isolation(run,runtime)
    try:
        isolation.begin()
        notify('Skyrim '+runtime+' is starting. Quit Skyrim normally when finished.')
        with (run/'manager.stdout.log').open('wb') as out,(run/'manager.stderr.log').open('wb') as err:
            process=subprocess.Popen([str(Path(cfg['manager'])/'ModOrganizer.exe'),'-p',profile,'run','-e','SKSE'],cwd=cfg['manager'],env=child_environment(),stdin=subprocess.DEVNULL,stdout=out,stderr=err,**hidden_options())
            record['managerPid']=process.pid;write_json(run/'run.json',record)
            record['managerExitCode']=process.wait()
        # Do not restore the shared catalog/logs while an actual game still runs.
        while any(n in ('skyrimse.exe','skse64_loader.exe') for n,p in process_names()):time.sleep(1)
    except Exception as e:
        record['error']=str(e)
        raise
    finally:
        record['endedAt']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        write_json(run/'run.json',record)
        # Retain the recovery journal if anything is still using shared files.
        ensure_idle()
        if isolation.record.exists():isolation.restore()
        ACTIVE.unlink(missing_ok=True)
        for folder in ('logs','overwrite'):
            p=Path(cfg['manager'])/folder
            if p.is_dir():shutil.copytree(p,run/('manager-'+folder),dirs_exist_ok=True)
        output=Path(cfg['manager'])/'mods'/spec['output']
        if output.is_dir():shutil.copytree(output,run/'settings-after',dirs_exist_ok=True)
    notify('Run saved. Mark the checks you actually completed.')
    return run

def result_summary(run):
    r=read_json(run/'run.json')
    return r.get('startup','unreviewed')+' / '+r.get('gameplay','not-run')

def gui():
    import tkinter as tk
    from tkinter import ttk,messagebox
    batch=read_json(MANIFESTS/'current-candidate.json') if (MANIFESTS/'current-candidate.json').is_file() else {}
    title=(batch.get('name','OmniCam')+' '+batch.get('version','')).strip()
    app=tk.Tk();app.title(title+' — Runtime Tests');app.geometry('850x580');app.minsize(740,520)
    events=queue.Queue();state={'busy':False,'run':None}
    frame=ttk.Frame(app,padding=18);frame.pack(fill='both',expand=True)
    ttk.Label(frame,text=title,font=('Segoe UI',18,'bold')).pack(anchor='w')
    ttk.Label(frame,text='Choose a Skyrim version. Every run keeps its own saves, settings, and results.').pack(anchor='w',pady=(4,12))
    row=ttk.Frame(frame);row.pack(fill='x')
    runtime=tk.StringVar();profile=tk.StringVar();choices={}
    ready=prepared_runtimes()
    box=ttk.Combobox(row,textvariable=runtime,values=ready,state='readonly',width=18);box.pack(side='left',padx=(0,10))
    profiles=ttk.Combobox(row,textvariable=profile,state='readonly',width=30);profiles.pack(side='left')
    status=tk.StringVar(value='Ready. Start with the main OmniCam profile.')
    detail=tk.StringVar()
    def select(*_):
        previous=choices.get(profile.get(),DEFAULT_PROFILE)
        cfg=load_config(runtime.get());choices.clear();choices.update(profile_choices(cfg))
        profiles['values']=list(choices)
        profile.set(next((label for label,name in choices.items() if name==previous),next(iter(choices))))
        detail.set(cfg.get('notes','Checks run before launch; saved results record what you tested.'))
    if ready:runtime.set('1.6.1170' if '1.6.1170' in ready else ready[0]);select()
    box.bind('<<ComboboxSelected>>',select)
    ttk.Label(frame,textvariable=detail,wraplength=790).pack(anchor='w',pady=(10,6))
    ttk.Label(frame,textvariable=status,wraplength=790,font=('Segoe UI',10,'bold')).pack(anchor='w',pady=(8,12))
    buttons=ttk.Frame(frame);buttons.pack(fill='x')
    def launch():
        if state['busy']:return
        state['busy']=True;start['state']='disabled';save['state']='disabled';state['run']=None
        startup.set('unreviewed');gameplay.set('not-run');notes.delete('1.0','end')
        rv=runtime.get();pr=choices[profile.get()]
        def worker():
            try:events.put(('done',run_session(rv,pr,lambda s:events.put(('status',s)))))
            except Exception as e:events.put(('error',str(e)))
        threading.Thread(target=worker,daemon=False).start()
    start=ttk.Button(buttons,text='Launch test',command=launch);start.pack(side='left')
    ttk.Button(buttons,text='Open checklist',command=lambda:os.startfile(ROOT/'CHECKLIST.md')).pack(side='left',padx=8)
    ttk.Button(buttons,text='Open results',command=lambda:os.startfile(ROOT/'runs')).pack(side='left')
    ttk.Separator(frame).pack(fill='x',pady=16)
    ttk.Label(frame,text='Logs save automatically. Select what passed or failed before saving your observations.',wraplength=790).pack(anchor='w',pady=(0,8))
    resultrow=ttk.Frame(frame);resultrow.pack(fill='x')
    ttk.Label(resultrow,text='Startup:').pack(side='left')
    startup=tk.StringVar(value='unreviewed');ttk.Combobox(resultrow,textvariable=startup,values=['unreviewed','pass','fail'],state='readonly',width=14).pack(side='left',padx=8)
    ttk.Label(resultrow,text='Gameplay:').pack(side='left',padx=(16,0))
    gameplay=tk.StringVar(value='not-run');ttk.Combobox(resultrow,textvariable=gameplay,values=['not-run','partial','pass','fail'],state='readonly',width=14).pack(side='left',padx=8)
    ttk.Label(frame,text='Notes — what you tested, what failed, and how to reproduce it:').pack(anchor='w',pady=(12,4))
    notes=tk.Text(frame,height=7,wrap='word',font=('Segoe UI',10));notes.pack(fill='both',expand=True)
    def save_result():
        run=state['run']
        if not run:return
        rec=read_json(run/'run.json');rec.update(startup=startup.get(),gameplay=gameplay.get(),notes=notes.get('1.0','end').strip())
        rec['reviewedAt']=datetime.datetime.now(datetime.timezone.utc).isoformat();write_json(run/'run.json',rec)
        if startup.get()=='unreviewed' and gameplay.get()=='not-run' and not rec['notes']:
            status.set('Logs are saved, but no test outcome was selected. Choose the results above and save again.')
        else:status.set('Saved your observations: '+run.name)
    save=ttk.Button(frame,text='Save result for this run',command=save_result,state='disabled');save.pack(anchor='e',pady=(8,0))
    def poll():
        while not events.empty():
            kind,value=events.get()
            if kind=='status':status.set(value)
            elif kind=='done':
                state.update(busy=False,run=value);start['state']='normal';save['state']='normal'
                completed=read_json(value/'run.json');save['text']='Save result: '+completed['runtime']+' / '+completed.get('profileLabel',completed['profile'])
                status.set('Test ended. Logs saved. Choose the outcomes below, then save: '+value.name)
            else:
                state['busy']=False;start['state']='normal';status.set(value);messagebox.showerror('Test could not finish',value)
        app.after(150,poll)
    def close():
        if state['busy']:messagebox.showinfo('Test running','Quit Skyrim first. Keep this window open so the original shared files can be restored.')
        else:app.destroy()
    app.protocol('WM_DELETE_WINDOW',close)
    try:
        recovered=recover_interrupted()
        if recovered:status.set('Recovered an interrupted session. Original shared files restored.')
    except Exception as e:status.set(str(e));start['state']='disabled'
    if not ready:start['state']='disabled';status.set('No complete runtime setups are available yet.')
    app.after(150,poll);app.mainloop()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--check-all',action='store_true');parser.add_argument('--full',action='store_true');parser.add_argument('--runtime');parser.add_argument('--profile',default=DEFAULT_PROFILE)
    args=parser.parse_args()
    if args.check_all:
        result=[]
        for runtime in prepared_runtimes():
            cfg=load_config(runtime)
            for profile in cfg['profiles']:
                try:check_setup(cfg['runtime'],profile['name'],args.full);result.append(dict(runtime=cfg['runtime'],profile=profile['name'],ok=True))
                except Exception as e:result.append(dict(runtime=cfg['runtime'],profile=profile['name'],ok=False,error=str(e)))
        write_json(MANIFESTS/'setup-validation.json',result)
        print(json.dumps(result));return 0 if result and all(r['ok'] for r in result) else 1
    if args.runtime:
        print(run_session(args.runtime,args.profile));return 0
    gui();return 0

if __name__=='__main__':
    try:sys.exit(main())
    except Exception as e:
        write_json(ROOT/'launcher-error.json',dict(error=str(e),time=datetime.datetime.now(datetime.timezone.utc).isoformat()))
        if not any(a.startswith('--') for a in sys.argv[1:]):
            ctypes.windll.user32.MessageBoxW(None,str(e),'OmniCam runtime tests',0x10)
        raise
