// Deterministic scheduler regression probe, not a browser performance benchmark.
const vm=require('node:vm'),fs=require('node:fs'),assert=require('node:assert/strict');
const source=fs.readFileSync(process.argv[2]||require('node:path').join(__dirname,'../Plugins/UnrealWebUI/Resources/regions-delta.js'),'utf8');
function probe(incremental){
  let callback,now=0,pointerEvents='auto',changedAt=null;
  const element={matches:()=>false,parentElement:null,getClientRects:()=>[{left:10,top:10,right:20,bottom:20}]};
  const context={__webuiIncremental:incremental,innerWidth:100,innerHeight:100,
    performance:{now:()=>now},requestAnimationFrame:fn=>callback=fn,
    getComputedStyle:()=>({pointerEvents,visibility:'visible'}),
    MutationObserver:class{observe(){}},ResizeObserver:class{observe(){}unobserve(){}},addEventListener(){},
    document:{querySelectorAll:()=>[element],getAnimations:()=>[],fonts:{addEventListener(){}}},
    __webuiNative:json=>{const p=JSON.parse(json);if(now>100&&(p.regions?.length===0||p.updates?.some(e=>e.rects.length===0)))changedAt??=now;}};
  context.window=context;vm.runInNewContext(source,context);
  now=100;callback(now);
  // CSSStyleSheet.insertRule changes computed style without a DOM mutation/resize.
  pointerEvents='none';
  for(now=120;now<=220;now+=20)callback(now);
  return changedAt;
}
const baseline=probe(false),incremental=probe(true);
assert.equal(baseline,120);assert.equal(incremental,process.argv[2]?200:120);
console.log(JSON.stringify({baselineObservedAt:baseline,incrementalObservedAt:incremental,extraDelayMs:incremental-baseline,
  decision:incremental===baseline?'same scan freshness':'100 ms audit prototype fails equivalent hit-region freshness'},null,2));
