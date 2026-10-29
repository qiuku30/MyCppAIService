const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname,'../../../AIApps/ChatServer/resource/AI.html'),'utf8');
const source = html.slice(html.indexOf('async function streamChat('), html.indexOf("chatForm.addEventListener('submit'"));
let chunks=[];
const span={innerHTML:''};
const context={TextDecoder,TextEncoder, Error,
 fetch:async()=>({ok:true,body:{getReader:()=>({read:async()=>chunks.length?{done:false,value:new TextEncoder().encode(chunks.shift())}:{done:true}})}}),
 document:{createElement:()=>({classList:{add(){}},querySelector:()=>span})},
 chatContainer:{appendChild(){},scrollTop:0,scrollHeight:0},
 DOMPurify:{sanitize:s=>s},marked:{parse:s=>s}};
vm.createContext(context);vm.runInContext(source,context);
async function rejects(input,pattern){chunks=input;await assert.rejects(context.streamChat('/fake',{}),pattern);}
(async()=>{
 await rejects(['data: {"error":"Gemma model not loaded"}\n\ndata: [DONE]\n\n'],/Gemma model not loaded/);
 console.log('PASS SSE errors reject');
 await rejects(['data: {"choices":[{"delta":{"content":"partial"}}]}\n\n'],/incomplete|完整|中断/i);
 console.log('PASS truncated stream rejects');
 await rejects(['data: [DONE]\n\n'],/empty|回答|内容/i);
 console.log('PASS empty completed stream rejects');
 chunks=['data: {"sessionId":"123"}\n\ndata: {"choices":[{"delta":{"content":"答"}}]}\n\n','data: {"choices":[{"delta":{"content":"案"}}]}\n\ndata: [DONE]\n\n'];
 const result=await context.streamChat('/fake',{});
 assert.equal(result.fullContent,'答案');assert.equal(result.newSessionId,'123');console.log('PASS completed stream returns text');
})().catch(e=>{console.error(e);process.exitCode=1;});
