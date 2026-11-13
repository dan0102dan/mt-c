import {
  buildImportedRules,
  parseRuleList,
  ruleListPreview,
  type ParsedRuleList,
  type RuleListRequest,
  type RuleListResponse,
} from "./rule-list-parser";

let prepared: { version: number; parsed: ParsedRuleList } | undefined;
self.onmessage = (event: MessageEvent<RuleListRequest>) => {
  const request = event.data;
  let response: RuleListResponse;
  try {
    if (request.type === "parse") {
      const parsed = parseRuleList(request.text, request.ruleType);
      prepared = { version: request.version, parsed };
      response = {
        id: request.id,
        version: request.version,
        type: "preview",
        preview: ruleListPreview(parsed),
      };
    } else {
      if (!prepared || prepared.version !== request.version)
        throw new Error("Rule list changed during import");
      response = {
        id: request.id,
        version: request.version,
        type: "rules",
        rules: buildImportedRules(prepared.parsed),
      };
    }
  } catch (error) {
    response = {
      id: request.id,
      version: request.version,
      type: "error",
      message: error instanceof Error ? error.message : String(error),
    };
  }
  self.postMessage(response);
};
