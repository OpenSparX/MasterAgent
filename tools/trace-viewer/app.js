const $ = selector => document.querySelector(selector);
const file = $('#file');
const form = $('#request-form');
const input = $('#request-text');
const send = $('#send');
const timeline = $('#timeline');
const detail = $('#detail');
const messages = $('#messages');
let events = [];
let startedAt = 0;
let timer;

const stageNames = {
  ingress:'接收用户请求',config:'绑定配置快照',preprocess:'输入预处理',memory:'召回记忆与上下文',
  rule_match:'规则与确定性能力匹配',local_classification:'端侧意图分类',retrieval:'召回 Skill 和能力',
  prompt_assembly:'组装模型提示词',local_inference:'端侧模型推理',decision_validation:'解析并校验模型决策',
  cloud_arbitration:'判断是否需要上云',cloud_inference:'云端模型推理',orchestration:'生成并提交执行计划',
  execution:'执行 Tool 或子 Agent',reconciliation:'核对执行结果',response:'生成用户回复',trace_finalize:'请求结束'
};
const eventNames = {
  TURN_ACCEPTED:'请求已接收',CONFIG_SNAPSHOT_BOUND:'配置已绑定',PREPROCESS_OPERATION_COMPLETED:'预处理模块完成',
  PREPROCESS_COMPLETED:'输入预处理完成',MEMORY_CONTEXT_RECALLED:'记忆召回完成',INTENT_SUBMITTED:'意图任务已提交',
  RULE_ROUTE_RESOLVED:'规则匹配完成',RETRIEVAL_RESOLVED:'Skill 与能力召回完成',LOCAL_INFERENCE_SKIPPED:'无需调用端侧模型',
  LOCAL_INFERENCE_OBSERVED:'端侧模型状态更新',CLOUD_ARBITRATION_SKIPPED:'无需上云',CLOUD_INFERENCE_SKIPPED:'未调用云端模型',
  CLOUD_ESCALATION_REQUESTED:'请求上云',CLOUD_ESCALATION_ALLOWED:'允许上云',CLOUD_ESCALATION_DENIED:'拒绝上云',
  CLOUD_PAYLOAD_SEALED:'上云数据已脱敏封装',CLOUD_FALLBACK_COMPLETED:'云端推理完成',INTENT_DECISION_VALIDATED:'执行决策校验通过',
  PLAN_COMMITTED:'执行计划已提交',ATOMIC_EXECUTION_OBSERVED:'Tool 执行状态更新',SUBAGENT_EXECUTION_OBSERVED:'子 Agent 状态更新',
  PLAN_OBSERVED:'执行结果已核对',TURN_COMPLETED:'请求成功结束',TURN_FAILED:'请求失败',TURN_PENDING:'请求仍在后台执行'
  ,REQUEST_RECEIVED:'收到原始请求',CONFIG_BOUND:'冻结本次配置',PREPROCESS_DETAIL:'预处理详细内容',
  MEMORY_RECALL_DETAIL:'记忆召回详细内容',CAPABILITY_RECALL_DETAIL:'可用能力召回详细内容',MODEL_INPUT:'模型收到完整输入',
  MODEL_OUTPUT:'模型返回原始输出',DECISION_DETAIL:'确定性决策详细内容',PLAN_DETAIL:'执行计划详细内容',
  EXECUTION_RESULT_DETAIL:'执行单元详细结果'
};
const statusNames = {SUCCEEDED:'成功',FAILED:'失败',STARTED:'进行中',SKIPPED:'已跳过',CANCELLED:'已取消',TIMEOUT:'超时',UNKNOWN:'结果未知',SUCCESS:'成功',DETAIL:'详细记录',LOCAL_DEBUG_ONLY:'仅限本地调试'};
const keyNames = {stage:'阶段',status:'状态',input:'输入摘要',output:'输出摘要',privacy:'隐私模式',type:'类型',length:'长度',digest:'摘要哈希',received:'收到的内容',operation:'处理动作',produced:'处理结果',reason:'原因与依据',text:'原始请求',original_text:'原始文本',normalized_text:'标准化文本',params:'参数',normalized_params:'标准化参数',session_id:'会话编号',turn_id:'轮次',request_id:'请求编号',trace_id:'链路编号',query:'召回查询',blocks:'召回片段',flattened_context:'拼接后的记忆上下文',content:'内容',source:'来源',relevance:'相关度',tools:'可用 Tool',name:'名称',title:'标题',description:'说明',input_schema:'输入格式',output_schema:'输出格式',catalog_digest:'能力目录摘要',prompt:'完整 Prompt',raw_output:'模型原始输出',protocol_version:'协议版本',phase:'推理阶段',finish_reason:'结束原因',outcome:'决策类型',plan:'执行计划',nodes:'执行节点',node_id:'节点编号',executor:'执行器',action:'能力名称',target_agent:'目标子 Agent',dependencies:'依赖节点',result:'执行结果',error:'错误',stage:'阶段',valid:'有效',context_blocks:'记忆片段数',capability_catalog_digest:'能力目录摘要',decision_type:'决策类型',reason_code:'决策原因',reply_digest:'回复摘要',user_reply:'用户回复',plan_id:'执行计划编号',node_count:'执行节点数',config_snapshot_id:'配置快照',job_id:'模型任务编号',model_stage:'模型阶段',model_id:'模型',model:'模型',runtime:'运行时',prompt_digest:'提示词摘要',model_output_digest:'模型输出摘要',prompt_tokens:'输入 Token',generated_tokens:'输出 Token',total_latency_ms:'模型耗时（毫秒）',tool_name:'执行能力',execution_id:'执行编号',side_effect_state:'副作用状态',result_digest:'结果摘要',agent_id:'子 Agent',reply_length:'回复长度',turn_summary:'请求结果',pending:'是否仍在执行'};

function escapeHtml(value){return String(value ?? '').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function setText(id,value){$(id).textContent=value;}
function translateObject(value){
  if(Array.isArray(value)) return value.map(translateObject);
  if(value&&typeof value==='object') return Object.fromEntries(Object.entries(value).map(([k,v])=>[keyNames[k]||k,translateObject(v)]));
  if(typeof value==='string') return statusNames[value]||stageNames[value]||value;
  return value;
}
function jsonText(value){
  if(value===undefined||value===null||value==='') return '这一步没有产生该项内容';
  return typeof value==='string'?value:JSON.stringify(translateObject(value),null,2);
}
function debugSections(event){
  const payload=event.payload||{};
  if(event.event_type==='MODEL_INPUT') return {
    received:{模型:payload.model,推理阶段:event.phase||payload.phase,Prompt分段:{System:payload.prompt_sections?.system,Skill候选:payload.prompt_sections?.skill_candidates,Tools:payload.prompt_sections?.tools,Memory:payload.prompt_sections?.memory,User:payload.prompt_sections?.user,输出协议:payload.prompt_sections?.protocol},完整Prompt:payload.prompt},
    operation:'把记忆、Skill、Tool、用户问题和输出协议组装后，提交给端侧模型推理。',
    produced:{模型任务编号:event.job_id,Prompt摘要:payload.prompt_digest},
    reason:'只有规则无法直接解决请求时才进入模型推理；完整 Prompt 是模型在这一刻真正看到的输入。'
  };
  if(event.event_type==='MODEL_OUTPUT') return {
    received:{模型:payload.model,模型任务编号:event.job_id},
    operation:'读取模型生成内容并交给决策解析器校验。',
    produced:{模型原始输出:payload.raw_output,结束原因:payload.finish_reason,输入Token:payload.prompt_tokens,输出Token:payload.generated_tokens,耗时毫秒:payload.total_latency_ms,运行时:payload.runtime,错误代码:payload.error_code},
    reason:'这里展示的是尚未转换成执行单元的模型原始输出，下一步还必须经过协议解析和安全校验。'
  };
  return {received:payload.received??payload.input??{},operation:payload.operation??event.operation??'记录该模块的处理过程',produced:payload.produced??payload.output??payload,reason:payload.reason??payload.reason_code??reasonFor(event)};
}
function reasonFor(event){
  if(event.status==='SKIPPED') return '当前请求不需要这个步骤，因此跳过。';
  if(event.event_type==='MEMORY_RECALL_DETAIL') return '为推理补充当前会话中可能相关的历史信息。';
  if(event.event_type==='CAPABILITY_RECALL_DETAIL') return '模型只能从已经注册且允许使用的能力中选择执行单元。';
  if(event.event_type==='DECISION_DETAIL') return event.payload?.produced?.reason_code||'将规则或模型结果约束为可校验、可执行的确定性结构。';
  return event.error_ref||'该步骤按当前链路配置和上一阶段结果执行。';
}
function previewFor(event){
  const payload=event.payload||{}, sections=debugSections(event);
  if(event.event_type==='REQUEST_RECEIVED') return payload.received?.text||'';
  if(event.event_type==='MEMORY_RECALL_DETAIL') return `召回 ${payload.produced?.blocks?.length||0} 个记忆片段`;
  if(event.event_type==='CAPABILITY_RECALL_DETAIL') return `发现 ${payload.produced?.tools?.length||0} 个可用 Tool`;
  if(event.event_type==='MODEL_INPUT') return String(payload.prompt||'').slice(0,100);
  if(event.event_type==='MODEL_OUTPUT') return String(payload.raw_output||'').slice(0,100);
  if(event.event_type==='DECISION_DETAIL') return `${payload.produced?.outcome||''} · ${payload.produced?.reason_code||''}`;
  return typeof sections.operation==='string'?sections.operation:'';
}
function addMessage(role,content,error=false){
  const hint=messages.querySelector('.hint'); if(hint) hint.remove();
  const node=document.createElement('div'); node.className=`message ${role} ${error?'error':''}`;
  node.innerHTML=`<span class="role">${role==='user'?'你':'MasterAgent'}</span>${escapeHtml(content)}`;
  messages.appendChild(node); messages.scrollTop=messages.scrollHeight;
}
function renderDetail(event){
  const sections=debugSections(event), local=event.local_debug?'<span class="local-only">原始内容 · 仅本地</span>':'';
  detail.innerHTML=`<div class="section-title"><span>步骤详情</span><small>${event.local_debug?'本地原始调试内容':'脱敏运行摘要'}</small></div>
    <h2>${escapeHtml(eventNames[event.event_type]||event.event_type||'内部事件')}${local}</h2>
    <dl><dt>处理阶段</dt><dd>${escapeHtml(stageNames[event.stage]||event.stage||'内部处理')}</dd>
    <dt>执行状态</dt><dd>${escapeHtml(statusNames[event.status]||event.status||event.outcome||'—')}</dd>
    <dt>所属模块</dt><dd>${escapeHtml(event.module||'—')}</dd><dt>具体操作</dt><dd>${escapeHtml(event.operation||'—')}</dd>
    <dt>执行计划</dt><dd>${escapeHtml(event.plan_id||'—')}</dd><dt>错误</dt><dd>${escapeHtml(event.error_ref||'无')}</dd></dl>
    <section class="io-block"><h3>① 接收到什么</h3><pre>${escapeHtml(jsonText(sections.received))}</pre></section>
    <section class="io-block"><h3>② 做了什么操作</h3><pre>${escapeHtml(jsonText(sections.operation))}</pre></section>
    <section class="io-block"><h3>③ 输出了什么</h3><pre>${escapeHtml(jsonText(sections.produced))}</pre></section>
    <section class="io-block reason"><h3>④ 为什么这样处理</h3><pre>${escapeHtml(jsonText(sections.reason))}</pre></section>`;
}
function addEvent(event){
  if(events.some(item=>item.event_id===event.event_id)) return;
  const hint=timeline.querySelector('.hint'); if(hint) hint.remove();
  events.push(event); const index=events.length-1; const item=document.createElement('li');
  const state=String(event.status||event.outcome||'').toUpperCase();
  item.className=`event ${event.local_debug?'debug':''} ${state==='FAILED'?'failed':state==='STARTED'?'running':state==='SKIPPED'?'skipped':''}`;
  const preview=previewFor(event);
  item.innerHTML=`<div class="stage">${escapeHtml(stageNames[event.stage]||event.stage||'内部处理')}</div><strong>${escapeHtml(eventNames[event.event_type]||event.event_type)}</strong><div class="meta">第 ${index+1} 步 · ${escapeHtml(statusNames[state]||state||'已记录')}${event.local_debug?' · 原始内容':''}</div>${preview?`<div class="preview">${escapeHtml(preview)}</div>`:''}`;
  item.onclick=()=>{document.querySelectorAll('.event').forEach(x=>x.classList.remove('active'));item.classList.add('active');renderDetail(event);};
  timeline.appendChild(item); timeline.parentElement.scrollTop=timeline.parentElement.scrollHeight;
  setText('#event-count',events.length); setText('#run-status',stageNames[event.stage]||'正在处理');
  if(index===0){item.classList.add('active');renderDetail(event);}
}
function resetRun(userText){events=[];timeline.innerHTML='';detail.innerHTML='<div class="section-title"><span>步骤详情</span><small>本地原始调试内容</small></div><p class="hint">执行开始后，点击步骤查看详细输入输出。</p>';setText('#event-count','0');setText('#result','处理中');setText('#trace-id','正在创建');setText('#run-status','请求已发送');addMessage('user',userText);startedAt=performance.now();clearInterval(timer);timer=setInterval(()=>setText('#duration',`${Math.round(performance.now()-startedAt)} 毫秒`),100);}
function finishRun(payload){clearInterval(timer);setText('#duration',`${Math.round(performance.now()-startedAt)} 毫秒`);setText('#trace-id',payload.trace_id||'—');setText('#result',payload.success?'成功':payload.pending?'仍在执行':'失败');setText('#run-status',payload.pending?'等待后台结果':'执行结束');addMessage('agent',payload.reply||payload.error_message||'没有返回内容',!payload.success&&!payload.pending);send.disabled=false;input.disabled=false;input.focus();}
async function readStream(response){
  if(!response.ok) throw new Error(await response.text());
  const reader=response.body.getReader(),decoder=new TextDecoder();let buffer='';
  while(true){const {done,value}=await reader.read();buffer+=decoder.decode(value||new Uint8Array(),{stream:!done});const lines=buffer.split('\n');buffer=lines.pop();for(const line of lines){if(!line.trim())continue;const message=JSON.parse(line);if(message.type==='event')addEvent(message.event);else if(message.type==='result')finishRun(message.result);else if(message.type==='error')throw new Error(message.message);}if(done)break;}
}
async function loadRuntime(){
  try{const response=await fetch('/api/health');if(!response.ok)throw new Error();const health=await response.json();$('#connection').textContent='本地服务已连接';$('#connection').classList.add('online');$('#runtime-config').textContent=`配置包：${health.config_bundle}　模型：${health.model_profile}　数据仅保存在：${health.runtime_root}`;send.disabled=false;}
  catch{$('#connection').textContent='未连接实时服务';$('#runtime-config').textContent='请在终端运行：python3 tools/trace-viewer/server.py';send.disabled=true;}
}
form.addEventListener('submit',async event=>{event.preventDefault();const request=input.value.trim();if(!request)return;resetRun(request);send.disabled=true;input.disabled=true;try{await readStream(await fetch('/api/run',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({text:request})}));}catch(error){clearInterval(timer);setText('#run-status','运行出错');setText('#result','失败');addMessage('agent',`实时面板运行失败：${error.message}`,true);send.disabled=false;input.disabled=false;}});
file.addEventListener('change',async()=>{try{const trace=JSON.parse(await file.files[0].text());if(trace.schema_version!=='masteragent.trace/v1alpha1'||!Array.isArray(trace.events))throw new Error('格式不支持');events=[];timeline.innerHTML='';trace.events.forEach(addEvent);setText('#trace-id',trace.trace_id||'—');setText('#result',statusNames[trace.events.at(-1)?.status]||'历史记录');setText('#run-status','查看历史链路');const first=trace.events[0]?.occurred_at_mono_ns||0,last=trace.events.at(-1)?.occurred_at_mono_ns||first;setText('#duration',`${Math.round((last-first)/1e6)} 毫秒`);}catch(error){alert(`无法打开链路文件：${error.message}`);}});
loadRuntime();
