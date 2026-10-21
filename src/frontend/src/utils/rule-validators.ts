export function isValidWildcard(pattern: string): boolean {
  return /^(?!\.)(?!.*\.$)(?!.*\.\.)(?!.*\*\*)[a-zA-Z0-9\-.*?]+$/.test(pattern);
}

export function isValidKeyword(pattern: string): boolean {
  if (!pattern.length || pattern.length > 4096) return false;
  for (let i = 0; i < pattern.length; i++) {
    const code = pattern.charCodeAt(i);
    if (code < 0x21 || code > 0x7e) return false;
  }
  return true;
}

export function isValidDomainWildcard(pattern: string): boolean {
  if (!isValidKeyword(pattern)) return false;
  for (const char of pattern) {
    if (
      !(char >= "a" && char <= "z") &&
      !(char >= "A" && char <= "Z") &&
      !(char >= "0" && char <= "9") &&
      !"-._*?".includes(char)
    ) {
      return false;
    }
  }
  return true;
}

export function isValidDomain(pattern: string): boolean {
  return /^(?!\.)(?!.*\.$)(?!.*\.\.)[a-zA-Z0-9\-.]+$/.test(pattern);
}

export function isValidNamespace(pattern: string): boolean {
  return isValidDomain(pattern);
}

export function isValidSubnet(pattern: string): boolean {
  let matches = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?:\/(\d{1,2}))?$/.exec(pattern);
  return !(
    !matches ||
    parseInt(matches[1]) > 255 ||
    parseInt(matches[2]) > 255 ||
    parseInt(matches[3]) > 255 ||
    parseInt(matches[4]) > 255 ||
    (matches[5] != "" && parseInt(matches[5]) > 32)
  );
}

function isValidIPv6(ip: string): boolean {
  return /^[0-9a-fA-F:]+$/.test(ip) && ip.includes(":");
}

export function isValidSubnet6(pattern: string): boolean {
  const parts = pattern.split("/");
  if (parts.length === 1) {
    return isValidIPv6(parts[0]);
  }

  if (parts.length !== 2) return false;

  const prefix = Number(parts[1]);
  if (!Number.isInteger(prefix) || prefix < 0 || prefix > 128) {
    return false;
  }

  return isValidIPv6(parts[0]);
}

export function isValidRegex(pattern: string): boolean {
  try {
    new RegExp(pattern);
    return true;
  } catch (e) {
    if (e instanceof SyntaxError) {
      return false;
    }
    return false;
  }
}

export const VALIDATOP_MAP: Record<string, (pattern: string) => boolean> = {
  regex: isValidRegex,
  wildcard: isValidWildcard,
  keyword: isValidKeyword,
  "domain-wildcard": isValidDomainWildcard,
  domain: isValidDomain,
  namespace: isValidNamespace,
  subnet: isValidSubnet,
  subnet6: isValidSubnet6,
};
