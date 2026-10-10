#!/usr/bin/env ruby
# Validates the public Swagger contract without downloading dependencies.
# Psych is bundled with Ruby on GitHub Actions' Ubuntu runners.
require "psych"

def fail_contract(message)
  abort("Invalid docs/swagger.yaml: #{message}")
end

def entries(node, context)
  fail_contract("#{context} is not a mapping") unless node.is_a?(Psych::Nodes::Mapping)
  items = {}
  node.children.each_slice(2) do |key, value|
    fail_contract("#{context} has a non-scalar key") unless key.is_a?(Psych::Nodes::Scalar)
    name = key.value
    fail_contract("#{context} has duplicate key #{name.inspect}") if items.key?(name)
    items[name] = value
    validate_tree(value, "#{context}/#{name}")
  end
  items
end

def validate_tree(node, context)
  case node
  when Psych::Nodes::Mapping
    entries(node, context)
  when Psych::Nodes::Sequence
    node.children.each_with_index { |child, i| validate_tree(child, "#{context}/#{i}") }
  end
end

def lookup(mapping, key, context)
  item = entries(mapping, context)[key]
  fail_contract("#{context} is missing #{key.inspect}") unless item
  item
end

begin
  document = Psych.parse_file(File.expand_path("../../docs/swagger.yaml", __dir__))
rescue Psych::SyntaxError => error
  fail_contract("YAML parse failed: #{error.message}")
end
fail_contract("empty document") unless document
root = entries(document.root, "root")
expected = %w[definitions info paths swagger]
fail_contract("unexpected root keys: #{root.keys.inspect}") unless root.keys.sort == expected.sort

definitions = entries(root.fetch("definitions"), "definitions")
%w[UpdateInfoRes UpdateInstallReq UpdateAcceptedRes UpdateStatusRes].each do |schema|
  fail_contract("missing definition types.#{schema}") unless definitions.key?("types.#{schema}")
end

paths = entries(root.fetch("paths"), "paths")
{
  "/api/v1/system/update" => ["get", %w[200 401 500]],
  "/api/v1/system/update/status" => ["get", %w[200 401 500]],
  "/api/v1/system/update/install" => ["post", %w[202 400 401 403 409 500]],
}.each do |path, (method, codes)|
  api = lookup(paths.fetch(path) { fail_contract("missing #{path}") }, method, path)
  responses = entries(lookup(api, "responses", "#{path}/#{method}"), "#{path}/#{method}/responses")
  codes.each do |code|
    fail_contract("missing #{code} response for #{method.upcase} #{path}") unless responses.key?(code)
  end
end

%w[UpdateAcceptedRes UpdateStatusRes].each do |type|
  schema = definitions.fetch("types.#{type}")
  properties = lookup(schema, "properties", type)
  job = lookup(properties, "job_id", type)
  pattern = lookup(job, "pattern", type)
  fail_contract("malformed #{type}.job_id.pattern") unless pattern.is_a?(Psych::Nodes::Scalar) &&
    pattern.value == '^[0-9a-f]{32}$'
end

puts "Swagger syntax, unique YAML keys and update API contract: OK"
