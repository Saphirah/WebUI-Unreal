import React, {useEffect, useState} from 'react';
import {createRoot} from 'react-dom/client';
import '../../../Plugins/UnrealWebUI/Resources/webui.js';
import './style.css';

function App() {
  const [status, setStatus] = useState('Connecting to Unreal…');
  const [count, setCount] = useState(0);
  const [reply, setReply] = useState(null);
  useEffect(() => {
    let active = true;
    window.ue.interface.setStatus = value => active && setStatus(value);
    window.webui.ready.then(async () => {
      if (!active) return;
      setStatus('React connected');
      await window.ue5('ready', {react:true, cssAnimation:true});
      const response = await window.webui.request('echo', {source:'React', nested:{items:[1,true,null]}}, 5);
      await window.ue5('roundtrip', response);
    }).catch(error => active && setStatus(error.message));
    return () => { active = false; delete window.ue.interface.setStatus; };
  }, []);
  async function send() {
    const next = count + 1; setCount(next);
    try { setReply(await window.webui.request('echo', {count:next, player:'世界', inventory:['key','map']}, 3)); }
    catch (error) { setReply({error:error.message}); }
  }
  return <main className="panel">
    <div className="pulse"/><h1>React + Unreal</h1><p>{status}</p>
    <label>HTML input <input placeholder="Keyboard / IME input"/></label>
    <button onClick={send}>JSON round trip · {count}</button>
    <pre>{JSON.stringify(reply, null, 2)}</pre>
    <p>The clear area around this panel passes mouse clicks to the game.</p>
  </main>;
}
createRoot(document.getElementById('root')).render(<App/>);
