import importlib.machinery,importlib.util,json,pathlib,tempfile,unittest,threading
from unittest.mock import patch
source=pathlib.Path(__file__).with_name('Runtime Tests.pyw')
loader=importlib.machinery.SourceFileLoader('launcher',str(source));spec=importlib.util.spec_from_loader(loader.name,loader);m=importlib.util.module_from_spec(spec);loader.exec_module(m)
class IsolationTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.temp.name);self.run=self.root/'run';self.run.mkdir()
  self.iso=m.Isolation(self.run,'1.5.97',self.root/'Documents',self.root/'Local');self.iso.cache=self.root/'runtime-cache'/'ContentCatalog.txt'
 def tearDown(self):self.temp.cleanup()
 def write(self,p,data):p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
 def test_original_files_restored_and_test_logs_preserved(self):
  self.write(self.iso.catalog,b'original catalog');self.write(self.iso.logroot/'DietDrCamera.log',b'old DDC');self.write(self.iso.logroot/'skse64.log',b'old SKSE');self.write(self.iso.logroot/'skse64_loader.log0',b'older loader')
  self.write(self.iso.cache,b'cached runtime catalog');self.iso.begin();self.assertEqual(self.iso.catalog.read_bytes(),b'cached runtime catalog');self.assertFalse((self.iso.logroot/'DietDrCamera.log').exists());self.assertFalse((self.iso.logroot/'skse64.log').exists())
  self.iso.catalog.write_bytes(b'generated catalog');self.write(self.iso.logroot/'DietDrCamera.log',b'new DDC');self.write(self.iso.logroot/'NewPlugin.log',b'new plugin');self.write(self.iso.logroot/'skse64_loader.log1',b'test rotation')
  self.iso.restore();self.assertEqual(self.iso.catalog.read_bytes(),b'original catalog');self.assertEqual(self.iso.cache.read_bytes(),b'generated catalog')
  self.assertEqual((self.iso.logroot/'DietDrCamera.log').read_bytes(),b'old DDC');self.assertEqual((self.iso.logroot/'skse64.log').read_bytes(),b'old SKSE');self.assertFalse((self.iso.logroot/'NewPlugin.log').exists());self.assertEqual((self.iso.logroot/'skse64_loader.log0').read_bytes(),b'older loader');self.assertFalse((self.iso.logroot/'skse64_loader.log1').exists())
  self.assertEqual((self.run/'after-shared'/'SKSE'/'DietDrCamera.log').read_bytes(),b'new DDC');self.assertEqual((self.run/'after-shared'/'SKSE'/'NewPlugin.log').read_bytes(),b'new plugin');self.assertEqual((self.run/'after-shared'/'SKSE'/'skse64_loader.log1').read_bytes(),b'test rotation')
  self.iso.restore();self.assertEqual(self.iso.catalog.read_bytes(),b'original catalog')
 def test_original_absence_restored(self):
  self.iso.begin();self.write(self.iso.catalog,b'generated');self.write(self.iso.logroot/'DietDrCamera.log',b'test');self.iso.restore()
  self.assertFalse(self.iso.catalog.exists());self.assertFalse((self.iso.logroot/'DietDrCamera.log').exists());self.assertEqual(self.iso.cache.read_bytes(),b'generated')
 def test_launch_failure_restores_catalog(self):
  self.write(self.iso.catalog,b'old');self.iso.begin();self.assertFalse(self.iso.catalog.exists());self.iso.restore();self.assertEqual(self.iso.catalog.read_bytes(),b'old')
 def test_changed_backup_refuses_restoration(self):
  self.write(self.iso.catalog,b'old');self.iso.begin();self.write(self.iso.catalog,b'new')
  state=m.read_json(self.iso.record);pathlib.Path(state['files'][0]['backup']).write_bytes(b'bad')
  with self.assertRaises(RuntimeError):self.iso.restore()
  self.assertEqual(self.iso.catalog.read_bytes(),b'new')
 def test_background_launch_flags(self):
  flags=m.hidden_options();self.assertFalse(flags['shell']);self.assertEqual(flags['creationflags'],m.subprocess.CREATE_NO_WINDOW)
  self.assertTrue(flags['startupinfo'].dwFlags & m.subprocess.STARTF_USESHOWWINDOW);self.assertEqual(flags['startupinfo'].wShowWindow,m.subprocess.SW_HIDE)
 def test_process_inventory_has_current_python(self):
  self.assertTrue(any(pid==m.os.getpid() for name,pid in m.process_names()))
class LauncherTests(unittest.TestCase):
 def test_session_lock_excludes_another_thread_and_releases(self):
  results=[]
  def attempt():
   try:
    with m.SessionLock():results.append('acquired')
   except RuntimeError:results.append('blocked')
  with m.SessionLock():
   t=threading.Thread(target=attempt);t.start();t.join(5);self.assertFalse(t.is_alive())
  t=threading.Thread(target=attempt);t.start();t.join(5);self.assertFalse(t.is_alive())
  self.assertEqual(results,['blocked','acquired'])
 def test_manager_retarget_and_loader_override_are_rejected(self):
  manager=m.ROOT/'managers'/'1.5.97';game=m.ROOT/'games'/'1.5.97'
  settings='\n'.join(k+'='+str(manager/f) for k,f in [('base_directory',''),('mod_directory','mods'),('profiles_directory','profiles'),('overwrite_directory','overwrite'),('download_directory','downloads')])
  settings+='\n1\\title=SKSE\n1\\binary='+str(game/'skse64_loader.exe')+'\n1\\workingDirectory='+str(game)+'\n1\\arguments=\n'
  m.check_manager_paths(settings,manager,game)
  with self.assertRaises(RuntimeError):m.check_manager_paths(settings.replace('mod_directory='+str(manager/'mods'),'mod_directory=C:/SkyrimMo2/mods'),manager,game)
  with self.assertRaises(RuntimeError):m.check_manager_paths(settings.replace('skse64_loader.exe','SkyrimSE.exe'),manager,game)
 def test_profile_normalization_preserves_enabled_flags_and_order(self):
  with tempfile.TemporaryDirectory() as tmp:
   p=pathlib.Path(tmp)/'plugins.txt';p.write_bytes(b'# MO2 comment\r\n*A.esp\r\nB.esp\r\n\r\n')
   self.assertEqual(m.profile_lines(p),['*A.esp','B.esp'])
 def test_check_all_ignores_its_own_previous_report(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=pathlib.Path(tmp);m.write_json(root/'setup-1.5.97.json',dict(runtime='1.5.97',profiles=[dict(name='DDC 1.2.1')]))
   with patch.object(m,'MANIFESTS',root),patch.object(m,'check_setup'),patch.object(m.sys,'argv',['launcher','--check-all']),patch('builtins.print'):
    self.assertEqual(m.main(),0);self.assertEqual(m.main(),0)
   self.assertEqual(len(m.read_json(root/'setup-validation.json')),1)
 def test_candidate_labels_keep_profile_and_integration_identity(self):
  cfg=dict(profiles=[dict(name='DDC 1.2.1',label='OmniCam 1.3.0'),dict(name='DDC 1.2.1 - TDM',label='OmniCam 1.3.0 - TDM'),dict(name='Baseline 1.2.0'),dict(name='Dependencies only')])
  self.assertEqual(m.profile_choices(cfg),{'OmniCam 1.3.0':'DDC 1.2.1','OmniCam 1.3.0 - TDM':'DDC 1.2.1 - TDM','Baseline 1.2.0':'Baseline 1.2.0','Dependencies only':'Dependencies only'})
 def test_check_all_uses_only_supported_prepared_versions(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=pathlib.Path(tmp)
   m.write_json(root/'targets.json',[dict(runtime='1.5.73',support='excluded'),dict(runtime='1.6.1170'),dict(runtime='1.6.640'),dict(runtime='1.6.1179')])
   for rv in ('1.5.73','1.6.1170','1.6.640'):
    m.write_json(root/('setup-'+rv+'.json'),dict(runtime=rv,profiles=[dict(name='DDC 1.2.1')]))
   with patch.object(m,'MANIFESTS',root),patch.object(m,'check_setup') as check,patch.object(m.sys,'argv',['launcher','--check-all']),patch('builtins.print'):
    self.assertEqual(m.prepared_runtimes(),['1.6.640','1.6.1170']);self.assertEqual(m.main(),0)
    self.assertEqual([call.args[0] for call in check.call_args_list],['1.6.640','1.6.1170'])
   self.assertEqual(len(m.read_json(root/'setup-validation.json')),2)
class FirstRunPreferencesTests(unittest.TestCase):
 def test_repairs_only_general_prompt_flags_and_preserves_encoding(self):
  with tempfile.TemporaryDirectory() as tmp:
   p=pathlib.Path(tmp)/'SkyrimPrefs.ini'
   p.write_bytes(b'\xef\xbb\xbf[Display]\r\nbFreebiesSeen=0\r\niSize W=1280\r\n[general]\r\n; custom comment\r\nbFREEBIESSeen=0\r\nbFreebiesSeen=0\r\nfCustom=2\r\n[AudioMenu]\r\nfAudioMasterVolume=0.25\r\n')
   before=p.read_bytes()
   with self.assertRaises(RuntimeError):m.prepare_preferences('1.6.1170',p)
   self.assertEqual(p.read_bytes(),before)
   self.assertTrue(m.prepare_preferences('1.6.1170',p,repair=True))
   result=p.read_bytes()
   self.assertTrue(result.startswith(b'\xef\xbb\xbf[Display]\r\nbFreebiesSeen=0\r\niSize W=1280\r\n'))
   self.assertIn(b'; custom comment\r\nfCustom=2\r\n[AudioMenu]\r\nfAudioMasterVolume=0.25\r\n',result)
   self.assertIn(b'[general]\r\nbFreebiesSeen=1\r\nbUpsellOwned=1\r\n',result)
   self.assertNotIn(b'\n',result.replace(b'\r\n',b''))
   self.assertFalse(m.prepare_preferences('1.6.1170',p,repair=True));self.assertEqual(p.read_bytes(),result)
 def test_missing_general_section_and_reset_are_repaired(self):
  with tempfile.TemporaryDirectory() as tmp:
   p=pathlib.Path(tmp)/'SkyrimPrefs.ini';p.write_bytes(b'[Display]\niSize W=1280')
   self.assertTrue(m.prepare_preferences('1.6.318',p,repair=True))
   self.assertEqual(p.read_bytes(),b'[Display]\niSize W=1280\n[General]\nbFreebiesSeen=1\n')
   p.write_bytes(p.read_bytes().replace(b'bFreebiesSeen=1',b'bFreebiesSeen=0'))
   with self.assertRaises(RuntimeError):m.prepare_preferences('1.6.318',p)
   self.assertTrue(m.prepare_preferences('1.6.318',p,repair=True));self.assertFalse(m.prepare_preferences('1.6.318',p))
 def test_older_versions_are_untouched_and_newer_versions_support_upsell(self):
  with tempfile.TemporaryDirectory() as tmp:
   p=pathlib.Path(tmp)/'SkyrimPrefs.ini';p.write_bytes(b'[General]')
   before=p.read_bytes()
   self.assertFalse(m.prepare_preferences('1.5.97',p,repair=True));self.assertEqual(p.read_bytes(),before)
   self.assertTrue(m.prepare_preferences('1.6.640',p,repair=True));self.assertNotIn(b'bUpsellOwned',p.read_bytes())
   self.assertTrue(m.prepare_preferences('1.7.104',p,repair=True));self.assertIn(b'bUpsellOwned=1',p.read_bytes())

class ResultFormTests(unittest.TestCase):
 def test_choices_entered_during_run_survive_completion(self):
  import tkinter as tk
  with tempfile.TemporaryDirectory() as tmp:
   root=pathlib.Path(tmp);manifests=root/'manifests';manifests.mkdir();run=root/'run';run.mkdir()
   m.write_json(manifests/'targets.json',[dict(runtime='1.5.73',support='excluded'),dict(runtime='1.6.1170')])
   m.write_json(manifests/'setup-1.5.73.json',dict(profiles=[dict(name='DDC 1.2.1')]))
   m.write_json(manifests/'setup-1.6.1170.json',dict(profiles=[dict(name='DDC 1.2.1',label='OmniCam 1.3.0')]))
   m.write_json(manifests/'current-candidate.json',dict(name='OmniCam',version='1.3.0'))
   m.write_json(run/'run.json',dict(runtime='1.6.1170',profile='DDC 1.2.1',startup='unreviewed',gameplay='not-run'))
   original=tk.Tk;scheduled=[]
   class InlineThread:
    def __init__(self,target,**kwargs):self.target=target
    def start(self):self.target()
   def hidden_root(*args,**kwargs):
    app=original(*args,**kwargs);app.withdraw();app.after=lambda delay,callback:scheduled.append(callback)
    def inspect():
     try:
      def walk(widget):
       yield widget
       for child in widget.winfo_children():yield from walk(child)
      widgets=list(walk(app));combos=[w for w in widgets if w.winfo_class()=='TCombobox']
      start=next(w for w in widgets if w.winfo_class()=='TButton' and w.cget('text')=='Launch test')
      save=next(w for w in widgets if w.winfo_class()=='TButton' and w.cget('text')=='Save result for this run')
      notes=next(w for w in widgets if w.winfo_class()=='Text')
      startup=next(w for w in combos if 'unreviewed' in w.cget('values'))
      gameplay=next(w for w in combos if 'partial' in w.cget('values'))
      startup.set('fail');gameplay.set('fail');notes.insert('1.0','prior run')
      self.assertEqual(app.title(),'OmniCam 1.3.0 — Runtime Tests')
      candidate=next(w for w in combos if 'OmniCam 1.3.0' in w.cget('values'))
      self.assertEqual(candidate.get(),'OmniCam 1.3.0')
      start.invoke();session.assert_called_once();self.assertEqual(session.call_args.args[:2],('1.6.1170','DDC 1.2.1'))
      self.assertEqual(startup.get(),'unreviewed');self.assertEqual(notes.get('1.0','end').strip(),'')
      startup.set('pass');gameplay.set('partial');notes.insert('1.0','POV and dialogue checked')
      scheduled.pop(0)()
      self.assertEqual(startup.get(),'pass');self.assertEqual(gameplay.get(),'partial')
      self.assertEqual(notes.get('1.0','end').strip(),'POV and dialogue checked')
      save.invoke();saved=m.read_json(run/'run.json')
      self.assertEqual(saved['startup'],'pass');self.assertEqual(saved['gameplay'],'partial')
      self.assertEqual(saved['notes'],'POV and dialogue checked');self.assertEqual(app.state(),'withdrawn')
     finally:app.destroy()
    app.mainloop=inspect;return app
   with patch.object(m,'ROOT',root),patch.object(m,'MANIFESTS',manifests),patch.object(m,'recover_interrupted',return_value=None),patch.object(m,'run_session',return_value=run) as session,patch.object(m.threading,'Thread',InlineThread),patch.object(tk,'Tk',hidden_root):m.gui()

if __name__=='__main__':unittest.main()

