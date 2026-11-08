import { isAlias, isMap, isScalar, isSeq, LineCounter, parseDocument, type Node } from "yaml";

import type { YamlConfigError } from "./config";
import { yamlConfigSchema, type YamlField } from "./schema";

/** Syntax and field types; the server remains responsible for configuration semantics.
 * Inspect the syntax tree without expanding aliases or rewriting the source. */
export function validateYaml(source: string): YamlConfigError[] {
  const lineCounter = new LineCounter();
  const document = parseDocument(source, { lineCounter, prettyErrors: false, intAsBigInt: true });
  const syntaxErrors = document.errors.map((error): YamlConfigError => {
    const position = lineCounter.linePos(error.pos[0]);
    return {
      message: error.message,
      line: position.line,
      column: position.col,
      from: error.pos[0],
      to: error.pos[1],
    };
  });
  if (!isMap(document.contents)) {
    return syntaxErrors.length
      ? syntaxErrors
      : [{ message: "Configuration must be a YAML mapping.", line: 1, column: 1 }];
  }
  function issue(node: Node | null, message: string): YamlConfigError {
    const from = node?.range?.[0] ?? 0;
    const position = lineCounter.linePos(from);
    return {
      message,
      from,
      to: node?.range?.[1] ?? from + 1,
      line: position.line,
      column: position.col,
    };
  }

  function check(node: Node | null, field: YamlField, depth = 0): YamlConfigError[] {
    if (depth > 16) return [issue(node, "Invalid or recursive YAML structure")];
    if (isAlias(node)) {
      const resolved = node.resolve(document);
      if (!resolved) return [issue(node, "Unknown YAML alias")];
      return check(resolved, field, depth + 1).map((error) => issue(node, error.message));
    }
    // Null means absent to the backend, except for explicitly typed priority.
    if ((!node || (isScalar(node) && node.value === null)) && field !== "priority") return [];
    if (typeof field !== "string") {
      const errors: YamlConfigError[] = [];
      if ("items" in field) {
        if (!isSeq(node)) return [issue(node, "Expected a YAML sequence")];
        for (const item of node.items) {
          errors.push(...check(item as Node | null, field.items, depth + 1));
        }
      } else {
        if (!isMap(node)) return [issue(node, "Expected a YAML mapping")];
        for (const pair of node.items) {
          if (!isScalar(pair.key) || !Object.hasOwn(field.fields, String(pair.key.value))) continue;
          errors.push(
            ...check(pair.value as Node | null, field.fields[String(pair.key.value)], depth + 1),
          );
        }
      }
      return errors;
    }
    if (!isScalar(node)) return [issue(node, "Expected a scalar value")];
    const plain = node.type === "PLAIN";
    const value = node.value;
    switch (field) {
      case "text":
        // yaml.v2 string fields accept scalar literal text, including numeric IDs.
        return [];
      case "boolean":
        // The backend uses YAML 1.1 boolean spellings, including yes/no/on/off.
        if (
          !plain ||
          !/^(?:true|True|TRUE|false|False|FALSE|yes|Yes|YES|no|No|NO|on|On|ON|off|Off|OFF|y|Y|n|N)$/.test(
            node.source ?? "",
          )
        )
          return [issue(node, "Expected a boolean value (true or false)")];
        return [];
      case "unsigned":
        if (
          !plain ||
          !(
            (typeof value === "bigint" && value >= 0n) ||
            (typeof value === "number" && Number.isInteger(value) && value >= 0)
          )
        )
          return [issue(node, "Expected a nonnegative integer")];
        return [];
      case "priority":
        if (!plain || typeof value !== "bigint" || value < 1n || value > 999n)
          return [issue(node, "Expected an integer from 1 to 999")];
        return [];
      case "duration":
        if (!(typeof value === "string" || (plain && typeof value === "bigint")))
          return [issue(node, "Expected a duration string or an integer")];
        return [];
    }
  }
  const errors = [...syntaxErrors, ...check(document.contents, yamlConfigSchema)];
  const seen = new Set<string>();
  return errors
    .filter((error) => {
      const key = `${error.from}:${error.to}:${error.message}`;
      if (seen.has(key)) return false;
      seen.add(key);
      return true;
    })
    .sort((a, b) => (a.from ?? 0) - (b.from ?? 0));
}
