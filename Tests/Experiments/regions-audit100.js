// A/B prototype. Both modes use identical conservative rectangle geometry.
(() => {
  if (window.__webuiRegions) return;
  window.__webuiRegions = true;
  const incremental = window.__webuiIncremental === true;
  const stats = window.__webuiRegionStats = {ms:0,reads:0,bytes:0,scans:0,samples:[]};
  let last=0, sequence=0, lastSent=0, audit=0, allDirty=true, topology=true, nextID=0, reset=true;
  let nodes=[];
  const cache=new Map(), dirty=new Set();
  const skip=e=>e.matches('#root,#app,[data-webui-root],script,style,link,meta');
  const invalidate=()=>{allDirty=true;};
  const resize=incremental?new ResizeObserver(invalidate):null;
  if(incremental){
    new MutationObserver(records=>{allDirty=true;if(records.some(r=>r.type==='childList'))topology=true;})
      .observe(document,{childList:true,subtree:true,attributes:true,characterData:true});
    for(const name of ['scroll','resize','pointerover','pointerout','focusin','focusout','load'])window.addEventListener(name,invalidate,true);
    document.fonts?.addEventListener('loadingdone',invalidate);
  }
  function collect(t){
    requestAnimationFrame(collect);
    if(t-last<1000/60)return;
    last=t;const start=performance.now();
    const styles=new WeakMap(),bounds=new WeakMap();
    const styleOf=e=>{let s=styles.get(e);if(!s){s=getComputedStyle(e);styles.set(e,s);}return s;};
    const boundsOf=e=>{let r=bounds.get(e);if(!r){r=e.getBoundingClientRect();bounds.set(e,r);}return r;};
    function rectangles(e){
      ++stats.reads;const result=[];
      const s=styleOf(e);if(s.pointerEvents==='none'||s.visibility!=='visible')return result;
      for(const r of e.getClientRects()){
        let left=Math.max(0,r.left),top=Math.max(0,r.top),right=Math.min(innerWidth,r.right),bottom=Math.min(innerHeight,r.bottom);
        for(let p=e.parentElement;p&&right>left&&bottom>top;p=p.parentElement){
          const s=styleOf(p);if(s.overflowX!=='visible'||s.overflowY!=='visible'){
            const r=boundsOf(p);
            if(s.overflowX!=='visible'){left=Math.max(left,r.left);right=Math.min(right,r.right);}
            if(s.overflowY!=='visible'){top=Math.max(top,r.top);bottom=Math.min(bottom,r.bottom);}
          }
        }
        if(right>left&&bottom>top)result.push([left/innerWidth,top/innerHeight,right/innerWidth,bottom/innerHeight]);
      }
      return result;
    }
    let packet;
    if(!incremental){
      const regions=[];
      for(const e of document.querySelectorAll('body *')){if(skip(e))continue;regions.push(...rectangles(e));if(regions.length>=8192)break;}
      packet={op:'regions',sequence:++sequence,width:innerWidth,height:innerHeight,regions,complete:regions.length<8192};
    }else{
      const removed=[],updates=[];
      // CSSOM edits and unobservable layout changes still need a bounded audit.
      if(t-audit>=100){allDirty=true;audit=t;}
      if(topology){
        nodes=[...document.querySelectorAll('body *')].filter(e=>!skip(e));
        const current=new Set(nodes);
        for(const [e,entry] of cache)if(!current.has(e)){removed.push(entry.id);cache.delete(e);resize.unobserve(e);}
        for(const e of nodes)if(!cache.has(e)){cache.set(e,{id:++nextID,rects:[],key:''});resize.observe(e);}
        topology=false;allDirty=true;
      }
      for(const animation of document.getAnimations()){
        if(animation.playState!=='running')continue;
        const effect=animation.effect,target=effect?.target;
        const composited=effect?.getKeyframes().every(k=>Object.keys(k).every(p=>['offset','computedOffset','easing','composite','transform','opacity','filter'].includes(p)));
        if(!composited||!target){allDirty=true;break;}
        dirty.add(target);for(const e of target.querySelectorAll('*'))dirty.add(e);
      }
      let count=0;
      for(const e of nodes){
        const entry=cache.get(e);
        if(allDirty||dirty.has(e)){
          const rects=rectangles(e),key=JSON.stringify(rects);
          if(key!==entry.key){entry.key=key;entry.rects=rects;updates.push({id:entry.id,rects});}
        }
        count+=entry.rects.length;
      }
      if(updates.length||removed.length||reset||t-lastSent>=100)
        packet={op:'regionsDelta',sequence:++sequence,reset,updates,removed,width:innerWidth,height:innerHeight,complete:count<8192&&nodes.length<=8192};
      allDirty=false;dirty.clear();reset=false;
    }
    if(packet){const json=JSON.stringify(packet);stats.bytes+=json.length;__webuiNative(json);lastSent=t;}
    const elapsed=performance.now()-start;stats.ms+=elapsed;++stats.scans;stats.samples.push(elapsed);
    if(stats.samples.length>2048)stats.samples.shift();
  }
  requestAnimationFrame(collect);
})();
