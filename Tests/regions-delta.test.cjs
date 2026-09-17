const {test}=require('node:test');
const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs');
const source=fs.readFileSync(require('node:path').join(__dirname,'../Plugins/UnrealWebUI/Resources/regions-delta.js'),'utf8');
function run(incremental){
  let frame,mutation,now=0,elements=[],packets=[],width=100;
  const context={__webuiIncremental:incremental,innerWidth:width,innerHeight:100,
    performance:{now:()=>now},requestAnimationFrame:f=>frame=f,
    getComputedStyle:e=>({pointerEvents:e.pointer||'auto',visibility:e.visibility||'visible'}),
    MutationObserver:class{constructor(f){mutation=f;}observe(){}},
    document:{querySelectorAll:()=>elements},__webuiNative:j=>packets.push(JSON.parse(j))};
  context.window=context;vm.runInNewContext(source,context);
  let cached=new Map(),result=[];
  const tick=()=>{now+=20;frame(now);for(const p of packets){
    if(p.regions)result=p.regions;
    else {if(p.reset)cached.clear();for(const id of p.removed)cached.delete(id);for(const u of p.updates)cached.set(u.id,u.rects);result=[...cached.values()].flat();}
  }packets=[];return JSON.stringify(result.slice().sort((a,b)=>a[0]-b[0]));};
  const node=x=>({skip:false,x,parentElement:null,matches(){return this.skip;},getClientRects(){return [{left:this.x,top:10,right:this.x+5,bottom:20}];}});
  const a=node(10),b=node(30);elements=[a,b];const snapshots=[tick()];
  a.x=15;snapshots.push(tick()); // Compositor geometry change without mutation.
  b.pointer='none';snapshots.push(tick()); // CSSOM rule changes without mutation.
  b.pointer='auto';snapshots.push(tick());
  a.skip=true;mutation?.([{type:'attributes',attributeName:'data-webui-root'}]);snapshots.push(tick());
  a.skip=false;mutation?.([{type:'attributes',attributeName:'id'}]);snapshots.push(tick());
  elements=[b];mutation?.([{type:'childList'}]);snapshots.push(tick());
  elements=[b,node(50)];mutation?.([{type:'childList'}]);snapshots.push(tick());
  b.visibility='hidden';snapshots.push(tick());
  return snapshots;
}
test('delta cache matches full snapshots after geometry, CSSOM, root markers, removal and addition',()=>{
  assert.deepEqual(run(true),run(false));
});
test('oversized element cache fails closed and recovers with a full reset',()=>{
  let frame,packets=[];
  const node=()=>({matches:()=>false,parentElement:null,getClientRects:()=>[{left:10,top:10,right:20,bottom:20}]});
  let elements=Array.from({length:8193},node);
  const context={__webuiIncremental:true,innerWidth:100,innerHeight:100,
    requestAnimationFrame:f=>frame=f,MutationObserver:class{observe(){}},
    getComputedStyle:()=>({pointerEvents:'auto',visibility:'visible'}),
    document:{querySelectorAll:()=>elements},__webuiNative:j=>packets.push(JSON.parse(j))};
  context.window=context;vm.runInNewContext(source,context);frame(20);
  assert.equal(packets.at(-1).complete,false);assert.equal(packets.at(-1).updates.length,0);
  elements=[node()];frame(40);
  assert.equal(packets.at(-1).reset,true);assert.equal(packets.at(-1).complete,true);assert.equal(packets.at(-1).updates.length,1);
});
