// Delta transport with geometry checked at the original scan cadence.
(() => {
  if (window.__webuiRegions) return;
  window.__webuiRegions = true;
  const incremental = window.__webuiIncremental === true;
  const profiling=window.__webuiAB===true;
  const stats = profiling?(window.__webuiRegionStats = {ms:0,reads:0,bytes:0,scans:0,samples:[]}):null;
  let last=0, sequence=0, lastSent=0, topology=true, nextID=0, reset=true;
  let lastWidth=0,lastHeight=0;
  let nodes=[];
  const cache=new Map();
  const skip=e=>e.matches('#root,#app,[data-webui-root],script,style,link,meta');
  if(incremental){
    new MutationObserver(records=>{if(records.some(r=>r.type==='childList'||r.attributeName==='id'||r.attributeName==='data-webui-root'))topology=true;})
      .observe(document,{childList:true,subtree:true,attributes:true,attributeFilter:['id','data-webui-root']});
  }
  function collect(t){
    requestAnimationFrame(collect);
    if(t-last<1000/60)return;
    last=t;const start=profiling?performance.now():0;
    const styles=new WeakMap(),bounds=new WeakMap();
    const styleOf=e=>{let s=styles.get(e);if(!s){s=getComputedStyle(e);styles.set(e,s);}return s;};
    const boundsOf=e=>{let r=bounds.get(e);if(!r){r=e.getBoundingClientRect();bounds.set(e,r);}return r;};
    function rectangles(e){
      if(profiling)++stats.reads;const result=[];
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
      // Recheck geometry every scan, including CSSOM changes and animations.
      if(topology){
        nodes=[];
        for(const e of document.querySelectorAll('body *')){if(!skip(e))nodes.push(e);if(nodes.length>8192)break;}
        if(nodes.length>8192){
          cache.clear();reset=true;
          __webuiNative(JSON.stringify({op:'regionsDelta',reset:true,updates:[],removed:[],width:innerWidth,height:innerHeight,complete:false}));
          return;
        }
        const current=new Set(nodes);
        for(const [e,entry] of cache)if(!current.has(e)){removed.push(entry.id);cache.delete(e);}
        for(const e of nodes)if(!cache.has(e))cache.set(e,{id:++nextID,rects:[],key:''});
        topology=false;
      }
      let count=0;
      for(const e of nodes){
        const entry=cache.get(e);
        {
          const rects=rectangles(e),key=JSON.stringify(rects);
          if(key!==entry.key){entry.key=key;entry.rects=rects;updates.push({id:entry.id,rects});}
        }
        count+=entry.rects.length;
        if(count>8192)break;
      }
      if(count>8192){
        cache.clear();topology=true;reset=true;
        packet={op:'regionsDelta',reset:true,updates:[],removed:[],width:innerWidth,height:innerHeight,complete:false};
      }else if(updates.length||removed.length||reset||innerWidth!==lastWidth||innerHeight!==lastHeight||t-lastSent>=100)
        packet={op:'regionsDelta',sequence:++sequence,reset,updates,removed,width:innerWidth,height:innerHeight,complete:count<8192&&nodes.length<=8192};
      if(count<=8192)reset=false;
    }
    if(packet){const json=JSON.stringify(packet);if(profiling)stats.bytes+=json.length;__webuiNative(json);lastSent=t;lastWidth=innerWidth;lastHeight=innerHeight;}
    if(profiling){const elapsed=performance.now()-start;stats.ms+=elapsed;++stats.scans;stats.samples.push(elapsed);
      if(stats.samples.length>2048)stats.samples.shift();}
  }
  requestAnimationFrame(collect);
})();
