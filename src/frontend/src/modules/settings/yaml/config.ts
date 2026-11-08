import { HttpError } from "../../../utils/http-error";

export type YamlConfig = {
  yaml: string;
  revision: string;
  restart_required: boolean;
  out_of_sync: boolean;
};

export type YamlConfigError = {
  message: string;
  code?: string;
  line?: number;
  column?: number;
};

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function readYamlConfig(value: unknown): YamlConfig {
  if (
    !record(value) ||
    typeof value.yaml !== "string" ||
    typeof value.revision !== "string" ||
    value.revision.length === 0 ||
    typeof value.restart_required !== "boolean" ||
    typeof value.out_of_sync !== "boolean"
  ) {
    throw new Error("Invalid YAML configuration response");
  }
  return {
    yaml: value.yaml,
    revision: value.revision,
    restart_required: value.restart_required,
    out_of_sync: value.out_of_sync,
  };
}

export function readYamlError(error: unknown): YamlConfigError {
  if (error instanceof HttpError && record(error.data)) {
    const value = error.data;
    return {
      message: typeof value.error === "string" ? value.error : "Request failed",
      ...(typeof value.code === "string" ? { code: value.code } : {}),
      ...(typeof value.line === "number" && Number.isInteger(value.line) && value.line > 0
        ? { line: value.line }
        : {}),
      ...(typeof value.column === "number" && Number.isInteger(value.column) && value.column > 0
        ? { column: value.column }
        : {}),
    };
  }
  return { message: error instanceof Error ? error.message : "Request failed" };
}
