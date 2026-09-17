// Conservative, local hit regions. No per-pointer JavaScript round trip.
// Rectangular DOM layout is covered; nonrectangular clipping remains conservative.
(() => {
  if (window.__webuiRegions) return;
  window.__webuiRegions = true;
  let last = 0, sequence = 0;
  function collect(t) {
    requestAnimationFrame(collect);
    if (t-last < 1000/60) return;
    last=t;
    const regions=[];
    const styles=new WeakMap(),bounds=new WeakMap();
    const styleOf=e=>{let s=styles.get(e);if(!s){s=getComputedStyle(e);styles.set(e,s)}return s;};
    const boundsOf=e=>{let r=bounds.get(e);if(!r){r=e.getBoundingClientRect();bounds.set(e,r)}return r;};
    for (const e of document.querySelectorAll('body *')) {
      if (e.matches('#root,#app,[data-webui-root],script,style,link,meta')) continue;
      const style=styleOf(e);
      if (style.pointerEvents==='none' || style.visibility!=='visible') continue;
      for (const rect of e.getClientRects()) {
        let left=Math.max(0,rect.left),top=Math.max(0,rect.top),right=Math.min(innerWidth,rect.right),bottom=Math.min(innerHeight,rect.bottom);
        for (let p=e.parentElement;p && right>left && bottom>top;p=p.parentElement) {
          const s=styleOf(p);
          if (s.overflowX!=='visible' || s.overflowY!=='visible') {
            const r=boundsOf(p);
            if (s.overflowX!=='visible') {left=Math.max(left,r.left);right=Math.min(right,r.right)}
            if (s.overflowY!=='visible') {top=Math.max(top,r.top);bottom=Math.min(bottom,r.bottom)}
          }
        }
        if(right>left && bottom>top) regions.push([left/innerWidth,top/innerHeight,right/innerWidth,bottom/innerHeight]);
      }
      if (regions.length>=8192) break;
    }
    __webuiNative(JSON.stringify({op:'regions',sequence:++sequence,width:innerWidth,height:innerHeight,regions,complete:regions.length<8192}));
  }
  requestAnimationFrame(collect);
})();
