"""Local desktop library and automatic Saturn disc preparation."""
from __future__ import annotations
import ast, base64, configparser, difflib, hashlib, json, os, re, shutil, subprocess, sys, threading, time, uuid
from pathlib import Path
import urllib.request, urllib.parse, urllib.error
import xml.etree.ElementTree as ET

ROOT = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parents[1]))
INSTALL = Path(sys.executable).resolve().parent if getattr(sys, 'frozen', False) else ROOT
PYTHON = sys.executable
CREATE_HIDDEN = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(value, indent=2), encoding='utf-8')
    tmp.replace(path)

def clean_title(value):
    return re.sub(r'\s+', ' ', re.sub(r'\([^)]*\)|\[[^]]*\]', '', value)).strip()

def odyssey_credentials():
    """Read the user's existing Odyssey integration; never serialize secrets."""
    values = {}
    config = Path(os.environ.get('APPDATA', Path.home())) / 'Odyssey/config.json'
    if config.exists():
        try: values = json.loads(config.read_text(encoding='utf-8-sig')).get('igdb', {})
        except (OSError, ValueError): pass
    source = Path.home() / 'Odyssey/odyssey/customize.py'
    fallback = {}
    if source.exists():
        try: nodes = ast.parse(source.read_text(encoding='utf-8-sig')).body
        except (OSError, SyntaxError): nodes = []
        for node in nodes:
            if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant):
                for target in node.targets:
                    if isinstance(target, ast.Name) and target.id in ('_IGDB_DEFAULT_ID', '_IGDB_DEFAULT_SECRET'):
                        fallback[target.id] = node.value.value
    return (values.get('client_id') or fallback.get('_IGDB_DEFAULT_ID'),
            values.get('client_secret') or fallback.get('_IGDB_DEFAULT_SECRET'))

def igdb_score(game, query):
    # Match Odyssey's canonical-release ranking, particularly short disc titles.
    normalize = lambda text: re.sub(r'[^a-z0-9]+', ' ', text.casefold()).strip()
    score = difflib.SequenceMatcher(None, normalize(query), normalize(game.get('name', ''))).ratio() * 100
    category = game.get('game_type', game.get('category', 0))
    if isinstance(category, dict): category = category.get('id', 0)
    score += 25 if category == 0 else 10 if category in (8, 9, 10) else 5 if category == 11 else -40
    score += min(game.get('total_rating_count', 0) or 0, 60) * .5
    if normalize(game.get('name', '')).startswith(normalize(query) + ' '): score += 20
    return score

class CoverArt:
    def __init__(self):
        self.token = None
        self.expires = 0
    def fetch(self, title, target):
        cid, secret = odyssey_credentials()
        if not cid or not secret:
            return {'source': 'IGDB', 'status': 'Odyssey credentials unavailable'}
        if not self.token or self.expires < time.time()+60:
            body = urllib.parse.urlencode({'client_id':cid,'client_secret':secret,'grant_type':'client_credentials'}).encode()
            req = urllib.request.Request('https://id.twitch.tv/oauth2/token', data=body, method='POST')
            with urllib.request.urlopen(req, timeout=20) as r:
                response = json.load(r)
            self.token = response['access_token']
            self.expires = time.time()+response.get('expires_in',3600)
        query = clean_title(title).replace('"','').replace('\\','')[:160]
        body = f'search "{query}"; where platforms = (32) & game_type = (0,8,9,10,11); fields name,cover.image_id,first_release_date,summary,url,game_type,total_rating_count; limit 25;'.encode()
        req = urllib.request.Request('https://api.igdb.com/v4/games',data=body,method='POST',headers={
            'Client-ID':cid,'Authorization':'Bearer '+self.token,'Content-Type':'text/plain','User-Agent':'SaturnRecomp/1.0'})
        with urllib.request.urlopen(req, timeout=20) as r:
            games = json.load(r)
        if not games:
            return {'source':'IGDB','status':'No Saturn cover found'}
        match = max(games, key=lambda g: igdb_score(g, query))
        image_id = match.get('cover',{}).get('image_id','')
        result = {'source':'IGDB','name':match.get('name',title),'summary':match.get('summary',''),
                  'url':match.get('url',''),'year':time.strftime('%Y',time.gmtime(match['first_release_date'])) if match.get('first_release_date') else ''}
        if re.fullmatch('[A-Za-z0-9_-]+',image_id):
            req = urllib.request.Request(f'https://images.igdb.com/igdb/image/upload/t_cover_big/{image_id}.jpg')
            with urllib.request.urlopen(req,timeout=20) as r:
                target.write_bytes(r.read(8*1024*1024))
            result['status']='Cover ready'
        else:
            result['status']='Cover unavailable'
        return result

class Library:
    def __init__(self, root=None):
        self.root = Path(root or INSTALL/'library').resolve()
        self.root.mkdir(parents=True,exist_ok=True)
        for name in ('games','.staging','runtime','bios'):
            (self.root/name).mkdir(exist_ok=True)
        self.settings_path = self.root/'settings.json'
        self.settings = json.loads(self.settings_path.read_text(encoding='utf-8')) if self.settings_path.exists() else {'bios':'','interpolation':False}
        self.jobs = {}
        self.lock = threading.RLock()
        self.import_lock = threading.Lock()
        self.art = CoverArt()
        self.igdb_available = all(odyssey_credentials())
        self.sync_runtime()
        ini = configparser.ConfigParser()
        ini.read(self.root/'settings.ini', encoding='utf-8')
        if ini.has_option('Video','Interpolation'):
            self.settings['interpolation'] = ini.get('Video','Interpolation') == '120'
        self.save_settings()
    def tool(self,name):
        for parent in (ROOT/'runtime',ROOT/'out/launcher-tools',ROOT/'out',ROOT/'runner'):
            p=parent/name
            if p.exists():return p
        raise RuntimeError(f'{name} is missing. Run the launcher build script.')
    def sync_runtime(self):
        target=self.root/'runtime'
        for name in ('saturnwin.exe','SDL2.dll'):
            choices=[ROOT/'runtime'/name,ROOT/'runner'/name,Path('C:/msys64/mingw64/bin')/name]
            source=next((x for x in choices if x.exists()),None)
            if not source:raise RuntimeError(f'Shared runtime file missing: {name}')
            out=target/name
            if not out.exists() or hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(out.read_bytes()).digest():
                shutil.copy2(source,out)
        shaders=ROOT/'runtime/shaders' if (ROOT/'runtime/shaders').exists() else ROOT/'runner/shaders'
        (target/'shaders').mkdir(exist_ok=True)
        for source in shaders.glob('*.spv'):shutil.copy2(source,target/'shaders'/source.name)
    def save_settings(self):
        atomic_json(self.settings_path,self.settings)
        ini=self.root/'settings.ini'
        ini.write_text('[Video]\nInterpolation='+('120' if self.settings.get('interpolation') else '0')+'\n',encoding='utf-8')
    def set_bios(self,path):
        source=Path(path).resolve()
        if not source.is_file() or source.stat().st_size!=512*1024:
            raise ValueError('Choose a 512 KB Sega Saturn BIOS dump.')
        raw=source.read_bytes()
        if b'SEGA' not in raw[:65536]:
            raise ValueError('This file does not look like a Sega Saturn BIOS dump.')
        target=self.root/'bios'/('saturn-'+hashlib.sha256(raw).hexdigest()[:12]+'.bin')
        if not target.exists():shutil.copy2(source,target)
        self.settings.update(bios=str(target),bios_name=source.name)
        self.save_settings()
        for folder in (self.root/'games').iterdir():
            record=folder/'game.json'
            if record.exists():self.write_config(folder,json.loads(record.read_text(encoding='utf-8')))
        return self.state()
    def set_interpolation(self,value):
        self.settings['interpolation']=bool(value)
        self.save_settings()
        return self.state()
    def set_selected_game(self, key):
        """Remember the focused title without changing the user's collection."""
        if key:
            folder = self.root/'games'/key
            if folder.parent.resolve() != (self.root/'games').resolve() or not (folder/'game.json').is_file():
                raise ValueError('This game is no longer in the library.')
        with self.lock:
            self.settings['selected_game'] = key
            self.save_settings()
        return True
    def dismiss_job(self, key):
        with self.lock:
            if key in self.jobs and self.jobs[key]['status'] != 'running':
                del self.jobs[key]
        return True
    def write_config(self,folder,record,output_folder=None):
        # TOML basic strings are JSON-compatible for these paths/titles.
        q=lambda s:json.dumps(str(s).replace('\\','/'),ensure_ascii=False)
        bios = os.path.relpath(self.settings['bios'], folder) if self.settings.get('bios') else ''
        text='[game]\nname = '+q(record['title'])+'\nprefix = '+q(record['id'])+'\nproduct_no = '+q(record['product'])+'\ndisc = '+q(record['disc'])+'\nbios = '+q(bios)+'\n\n'
        text+='[[module]]\nname = "main"\nfile = '+q(record['module'])+'\ncpu = "sh2"\ncompression = "none"\nload_addr = '+record['load_address']+'\nentry = '+record['load_address']+'\nfirst_read = true\n'
        ((output_folder or folder)/'game.toml').write_text(text,encoding='utf-8')
    def state(self, include_art=True):
        """Return library metadata; native views load art directly from its path."""
        with self.lock:
            ini=configparser.ConfigParser()
            ini.read(self.root/'settings.ini',encoding='utf-8')
            if ini.has_option('Video','Interpolation'):self.settings['interpolation']=ini.get('Video','Interpolation')=='120'
            games=[]
            for p in (self.root/'games').glob('*/game.json'):
                g=json.loads(p.read_text(encoding='utf-8'))
                art=p.parent/'cover.jpg'
                g['cover_path']=str(art) if art.exists() else ''
                g['cover']='data:image/jpeg;base64,'+base64.b64encode(art.read_bytes()).decode() if include_art and art.exists() else ''
                g['folder']=str(p.parent)
                games.append(g)
            logo=ROOT/'assets/saturnrecomp-logo.png'
            return {'settings':dict(self.settings),'games':sorted(games,key=lambda g:g['title'].casefold()),
                    'jobs':[dict(j) for j in self.jobs.values()], 'library':str(self.root),
                    'igdb':self.igdb_available, 'logo_path':str(logo) if logo.exists() else '',
                    'logo':'data:image/png;base64,'+base64.b64encode(logo.read_bytes()).decode() if include_art and logo.exists() else ''}
    def start_import(self,path):
        disc=Path(path).resolve()
        if not disc.is_file() or disc.suffix.lower() not in ('.cue','.iso','.bin','.img'):
            raise ValueError('Choose a CUE, ISO, BIN or IMG Saturn disc.')
        if not self.settings.get('bios'):raise ValueError('Choose your BIOS in Console settings first.')
        for g in self.state(include_art=False)['games']:
            if Path(g['disc'])==disc:return {'existing':g['id']}
        job=uuid.uuid4().hex
        with self.lock:self.jobs[job]={'id':job,'title':clean_title(disc.stem),'phase':'Queued','progress':0,'status':'running'}
        threading.Thread(target=self._import,args=(job,disc),daemon=True).start()
        return {'job':job}
    def _update(self,job,**values):
        with self.lock:self.jobs[job].update(values)
    def _import(self,job,disc):
        with self.import_lock:
            stage=self.root/'.staging'/job
            stage.mkdir()
            try:
                self._update(job,phase='Reading disc and dumping assets',progress=2)
                log=stage/'import.log'
                with log.open('w',encoding='utf-8') as errors:
                    proc=subprocess.Popen([str(self.tool('saturn-import.exe')),str(disc),str(stage)],stdout=subprocess.PIPE,stderr=errors,text=True,creationflags=CREATE_HIDDEN)
                    for line in proc.stdout:
                        m=re.fullmatch(r'FILE (\d+) (\d+)\s*',line)
                        if m:self._update(job,progress=5+int(int(m[1])*65/max(1,int(m[2]))),phase=f'Dumping assets · {m[1]} / {m[2]} files')
                    proc.stdout.close()
                    if proc.wait():raise RuntimeError(log.read_text(encoding='utf-8',errors='replace').strip()[-1000:] or 'Disc extraction failed.')
                manifest=ET.parse(stage/'manifest.xml').getroot()
                title=' '.join(manifest.attrib['title'].split()) or disc.stem
                product=manifest.attrib['product'].strip()
                key=re.sub(r'[^a-z0-9]+','-',(title+'-'+product).casefold()).strip('-')[:90]
                files=list(manifest.find('files'))
                first=manifest.attrib.get('boot-file')
                if not first:raise RuntimeError('No boot module was found in the ISO root directory.')
                self._update(job,title=title,phase='Writing game manifest and executable',progress=74)
                record={'id':key,'title':title,'product':product,'areas':manifest.attrib['areas'],'version':manifest.attrib['version'],
                        'disc':str(disc),'module':first,'load_address':manifest.attrib['load-address'],'file_count':len(files),
                        'asset_bytes':sum(int(f.attrib['size']) for f in files),'created':time.time(),'execution':'shared-runtime','verification':'Not yet verified'}
                # The native executable and XML are per-title; execution stays in the shared hardware runtime.
                shutil.copy2(self.tool('saturn-game.exe'),stage/(key+'.exe'))
                self._update(job,phase='Fetching Saturn box art from IGDB',progress=87)
                try:record['metadata']=self.art.fetch(title,stage/'cover.jpg')
                except (OSError,ValueError,KeyError,urllib.error.URLError):record['metadata']={'source':'IGDB','status':'Cover unavailable; game import is complete'}
                atomic_json(stage/'game.json',record)
                final=self.root/'games'/key
                if not final.resolve().is_relative_to((self.root/'games').resolve()):raise RuntimeError('Invalid library destination.')
                if final.exists():raise RuntimeError('This edition is already in the library.')
                self.write_config(final, record, output_folder=stage)
                stage.replace(final)
                self._update(job,phase='Ready to launch',progress=100,status='complete',game=key)
            except Exception as e:
                self._update(job,phase=str(e),status='error',log=str(stage/'import.log'))
    def launch(self,key):
        folder=self.root/'games'/key
        if folder.parent.resolve()!=(self.root/'games').resolve():raise ValueError('Invalid game.')
        record=json.loads((folder/'game.json').read_text(encoding='utf-8'))
        if not Path(record['disc']).is_file():raise ValueError('The original disc was moved. Restore it before launching.')
        if not self.settings.get('bios') or not Path(self.settings['bios']).is_file():
            raise ValueError('Choose your Saturn BIOS in Console settings before playing.')
        self.sync_runtime()
        self.write_config(folder,record)
        env=os.environ.copy()
        for k in list(env):
            if k.startswith('SATURN_') or k=='SDL_AUDIODRIVER':env.pop(k,None)
        env['SATURN_SMPCFILE']=str(folder/'console.bin')
        proc=subprocess.Popen([str(folder/(key+'.exe'))],cwd=folder,env=env,creationflags=CREATE_HIDDEN)
        record['last_played'] = time.time()
        result = {'launched':True,'pid':proc.pid}
        try:
            atomic_json(folder/'game.json', record)
        except OSError:
            result['warning'] = 'The game started, but its last-played time could not be saved.'
        return result
    def open_folder(self,key=''):
        folder=self.root/'games'/key if key else self.root
        if not folder.resolve().is_relative_to(self.root):raise ValueError('Invalid folder.')
        os.startfile(folder)
        return True
