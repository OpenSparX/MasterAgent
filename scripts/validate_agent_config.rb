#!/usr/bin/env ruby

require "json"
require "pathname"
require "set"
require "yaml"

root = Pathname.new(ARGV.fetch(0, "agent-config")).expand_path
errors = []

def load_yaml(path, errors)
  YAML.safe_load(
    path.read,
    permitted_classes: [],
    permitted_symbols: [],
    aliases: false
  )
rescue StandardError => error
  errors << "#{path}: YAML 无法解析：#{error.message}"
  nil
end

def resources(root, directory, errors)
  root.join(directory).glob("*.yaml").sort.map do |path|
    document = load_yaml(path, errors)
    [path, document] if document.is_a?(Hash)
  end.compact
end

unless root.join("manifest.yaml").file?
  warn "配置包不存在 manifest.yaml：#{root}"
  exit 1
end

manifest = load_yaml(root.join("manifest.yaml"), errors)
unless manifest&.dig("kind") == "MasterAgentConfigBundle"
  errors << "manifest.yaml: kind 必须为 MasterAgentConfigBundle"
end
unless manifest&.dig("api_version") == "masteragent.ai/v1alpha1"
  errors << "manifest.yaml: api_version 必须为 masteragent.ai/v1alpha1"
end

skill_docs = resources(root, "skills", errors)
capability_docs = resources(root, "capabilities", errors)
context_docs = resources(root, "context_sources", errors)
agent_docs = resources(root, "agents", errors)
model_docs = resources(root, "models", errors)
rule_docs = resources(root, "rules", errors)
policy_docs = resources(root, "policies", errors)
test_docs = resources(root, "tests", errors)

expected_kinds = {
  "skills" => ["Skill"],
  "capabilities" => ["CapabilitySet"],
  "context_sources" => ["ContextSourceSet"],
  "agents" => ["Agent"],
  "models" => ["ModelProfile"],
  "rules" => ["RuleSet"],
  "policies" => ["PolicySet", "ModelRoutingPolicy", "ObservabilityPolicy"],
  "tests" => ["ConfigTestSuite"]
}
expected_kinds.each do |directory, kinds|
  resources(root, directory, errors).each do |path, document|
    unless document["api_version"] == "masteragent.ai/v1alpha1"
      errors << "#{path}: api_version 必须为 masteragent.ai/v1alpha1"
    end
    unless kinds.include?(document["kind"])
      errors << "#{path}: kind 必须为 #{kinds.join(" 或 ")}"
    end
  end
end

skill_ids = skill_docs.map { |_, doc| doc.dig("metadata", "id") }.compact.to_set
capability_ids = capability_docs.flat_map do |_, doc|
  Array(doc.dig("spec", "capabilities")).map { |item| item["id"] }
end.compact.to_set
context_ids = context_docs.flat_map do |_, doc|
  Array(doc.dig("spec", "sources")).map { |item| item["id"] }
end.compact.to_set
agent_ids = agent_docs.map { |_, doc| doc.dig("metadata", "id") }.compact.to_set
model_ids = model_docs.map { |_, doc| doc.dig("metadata", "id") }.compact.to_set
test_suite_ids = test_docs.map { |_, doc| doc.dig("metadata", "id") }.compact.to_set
models_by_id = model_docs.each_with_object({}) do |(path, document), index|
  id = document.dig("metadata", "id")
  index[id] = [path, document] if id
end

[
  ["Skill", skill_docs.map { |_, doc| doc.dig("metadata", "id") }],
  ["Capability", capability_docs.flat_map { |_, doc| Array(doc.dig("spec", "capabilities")).map { |item| item["id"] } }],
  ["ContextSource", context_docs.flat_map { |_, doc| Array(doc.dig("spec", "sources")).map { |item| item["id"] } }],
  ["Agent", agent_docs.map { |_, doc| doc.dig("metadata", "id") }],
  ["Model", model_docs.map { |_, doc| doc.dig("metadata", "id") }],
  ["ConfigTestSuite", test_docs.map { |_, doc| doc.dig("metadata", "id") }]
].each do |label, ids|
  counts = Hash.new(0)
  ids.compact.each { |id| counts[id] += 1 }
  counts.each do |id, count|
    errors << "#{label} ID 重复：#{id}" if count > 1
  end
end

model_docs.each do |path, document|
  spec = document.fetch("spec", {})
  unless ["intent", "classifier", "embedding", "reranker", "summarizer"].include?(spec["role"])
    errors << "#{path}: 未知模型 role #{spec["role"].inspect}"
  end
  unless ["on_device", "cloud"].include?(spec["location"])
    errors << "#{path}: location 必须为 on_device 或 cloud"
  end
  errors << "#{path}: runtime.type 不能为空" if spec.dig("runtime", "type").to_s.empty?
  unless ["real", "cloud", "simulated", "simulated_cloud"].include?(spec["reality"])
    errors << "#{path}: 未知 reality #{spec["reality"].inspect}"
  end
  if spec["location"] == "cloud" && spec.dig("artifact", "path")
    errors << "#{path}: 云端模型不能嵌入本地权重路径"
  end
  [spec.dig("artifact", "path_ref"),
   spec.dig("runtime", "endpoint_ref"),
   spec.dig("runtime", "credential_ref"),
   spec.dig("model", "name_ref")].compact.each do |reference|
    unless reference.match?(/\A[A-Z][A-Z0-9_]*\z/)
      errors << "#{path}: 外部引用必须是环境变量名：#{reference}"
    end
  end
  protocol = spec["protocol"]
  if protocol.is_a?(Hash)
    ["prompt_profile", "output_schema"].each do |field|
      reference = protocol[field]
      next if reference.to_s.empty?
      errors << "#{path}: #{field} 引用了不存在的文件 #{reference}" unless root.join(reference).file?
    end
  end
  Array(spec.dig("validation", "required_test_suites")).each do |suite_id|
    unless test_suite_ids.include?(suite_id)
      errors << "#{path}: 引用了未知 ConfigTestSuite #{suite_id}"
    end
  end
end

model_routing_ids = policy_docs
  .select { |_, document| document["kind"] == "ModelRoutingPolicy" }
  .map { |_, document| document.dig("metadata", "id") }
  .compact
  .to_set
default_model_routing = manifest&.dig("spec", "defaults", "model_routing")
if default_model_routing && !model_routing_ids.include?(default_model_routing)
  errors << "manifest.yaml: model_routing 引用了未知 ModelRoutingPolicy #{default_model_routing}"
end

observability_ids = policy_docs
  .select { |_, document| document["kind"] == "ObservabilityPolicy" }
  .map { |_, document| document.dig("metadata", "id") }
  .compact
  .to_set
default_observability = manifest&.dig("spec", "defaults", "observability_policy")
if default_observability && !observability_ids.include?(default_observability)
  errors << "manifest.yaml: observability_policy 引用了未知 ObservabilityPolicy #{default_observability}"
end

policy_docs.select { |_, document| document["kind"] == "ObservabilityPolicy" }.each do |path, document|
  spec = document.fetch("spec", {})
  unless ["safe", "local-debug", "audit"].include?(spec["mode"])
    errors << "#{path}: mode 必须为 safe、local-debug 或 audit"
  end
  if spec.dig("payloads", "allow_credentials") == true
    errors << "#{path}: 不允许记录 credentials"
  end
end

policy_docs.select { |_, document| document["kind"] == "ModelRoutingPolicy" }.each do |path, document|
  roles = document.dig("spec", "roles") || {}
  roles.each do |role, route|
    default_id = route["default"]
    unless model_ids.include?(default_id)
      errors << "#{path}: #{role}.default 引用了未知 Model #{default_id}"
      next
    end
    default_model = models_by_id.fetch(default_id)[1]
    errors << "#{path}: #{default_id} 的 role 与路由 #{role} 不一致" unless default_model.dig("spec", "role") == role
    errors << "#{path}: 默认 Model #{default_id} 未启用" unless default_model.dig("spec", "enabled") == true
    unless Array(route["on_device_candidates"]).include?(default_id)
      errors << "#{path}: #{role}.default 必须出现在 on_device_candidates"
    end

    {
      "on_device_candidates" => "on_device",
      "cloud_candidates" => "cloud"
    }.each do |field, expected_location|
      Array(route[field]).each do |model_id|
        unless model_ids.include?(model_id)
          errors << "#{path}: #{role}.#{field} 引用了未知 Model #{model_id}"
          next
        end
        model = models_by_id.fetch(model_id)[1]
        errors << "#{path}: #{model_id} 的 role 与路由 #{role} 不一致" unless model.dig("spec", "role") == role
        errors << "#{path}: #{model_id} 不属于 #{expected_location}" unless model.dig("spec", "location") == expected_location
      end
    end
  end
end

skill_docs.each do |path, document|
  spec = document.fetch("spec", {})
  Array(spec["allowed_context_sources"]).each do |id|
    errors << "#{path}: 未知 ContextSource #{id}" unless context_ids.include?(id)
  end
  capabilities = spec.fetch("allowed_capabilities", {})
  (Array(capabilities["read"]) + Array(capabilities["write"])).each do |id|
    errors << "#{path}: 未知 Capability #{id}" unless capability_ids.include?(id)
  end
  Array(spec["allowed_agents"]).each do |id|
    errors << "#{path}: 未知 Agent #{id}" unless agent_ids.include?(id)
  end
end

rule_docs.each do |path, document|
  Array(document.dig("spec", "rules")).each do |rule|
    Array(rule.dig("decision", "actions")).each do |action|
      id = action["capability"]
      errors << "#{path}: 规则 #{rule["id"]} 引用了未知 Capability #{id}" unless capability_ids.include?(id)
    end
  end
end

context_docs.each do |path, document|
  Array(document.dig("spec", "sources")).each do |source|
    Array(source.dig("access", "allowed_skills")).each do |id|
      next if id == "*" || skill_ids.include?(id)
      errors << "#{path}: ContextSource #{source["id"]} 引用了未知 Skill #{id}"
    end
  end
end

test_docs.each do |path, document|
  Array(document.dig("spec", "cases")).each do |test_case|
    expected = test_case.fetch("expect", {})
    Array(expected["allowed_skills"]).each do |id|
      errors << "#{path}: 测试 #{test_case["id"]} 引用了未知 Skill #{id}" unless skill_ids.include?(id)
    end
    Array(expected["allowed_capabilities"]).each do |id|
      errors << "#{path}: 测试 #{test_case["id"]} 引用了未知 Capability #{id}" unless capability_ids.include?(id)
    end
    Array(expected["allowed_agents"]).each do |id|
      errors << "#{path}: 测试 #{test_case["id"]} 引用了未知 Agent #{id}" unless agent_ids.include?(id)
    end
    [test_case["model_output"], test_case["cloud_output"]].compact.each do |output|
      next unless output.is_a?(Hash)
      Array(output["actions"]).each do |action|
        next unless action["type"] == "CallTool"
        capability_id = action["capability"]
        unknown_is_expected =
          expected["local_validation"] == "rejected" &&
          expected["error_code"] == "CAPABILITY_NOT_REGISTERED"
        if !unknown_is_expected && !capability_ids.include?(capability_id)
          errors << "#{path}: 测试 #{test_case["id"]} 的模型输出引用未知 Capability #{capability_id}"
        end
      end
    end
  end
end

root.glob("**/*.json").sort.each do |path|
  JSON.parse(path.read)
rescue JSON::ParserError => error
  errors << "#{path}: JSON 无法解析：#{error.message}"
end

unless errors.empty?
  warn errors.join("\n")
  exit 1
end

puts "MasterAgent 配置包校验通过：#{root}"
puts "skills=#{skill_ids.size} capabilities=#{capability_ids.size} contexts=#{context_ids.size} agents=#{agent_ids.size} models=#{model_ids.size}"
