import type { ReactNode, SVGProps } from "react";

export type CemuIconName =
  | "account"
  | "box"
  | "check"
  | "controller"
  | "copy"
  | "download"
  | "display"
  | "folder"
  | "grid"
  | "help"
  | "image"
  | "key"
  | "link"
  | "library"
  | "list"
  | "mods"
  | "play"
  | "refresh"
  | "search"
  | "settings"
  | "audio"
  | "terminal"
  | "tools"
  | "warning";

const paths: Record<CemuIconName, ReactNode> = {
  account: (
    <>
      <path d="M20 21a8 8 0 0 0-16 0" />
      <circle cx="12" cy="8" r="5" />
    </>
  ),
  box: (
    <>
      <path d="m4 7 8-4 8 4v10l-8 4-8-4z" />
      <path d="m4 7 8 4 8-4M12 11v10" />
    </>
  ),
  check: <path d="m5 12 4 4L19 6" />,
  controller: (
    <>
      <path d="M7 8h10a4 4 0 0 1 3.8 5.2l-1.3 4A2.5 2.5 0 0 1 15 18l-2-2h-2l-2 2a2.5 2.5 0 0 1-4.5-.8l-1.3-4A4 4 0 0 1 7 8z" />
      <path d="M8 11v4m-2-2h4M16 12h.01M18 14h.01" />
    </>
  ),
  copy: (
    <>
      <rect x="8" y="8" width="12" height="12" rx="1" />
      <path d="M16 8V4H4v12h4" />
    </>
  ),
  download: (
    <>
      <path d="M12 3v12m0 0 4-4m-4 4-4-4" />
      <path d="M5 20h14" />
    </>
  ),
  display: (
    <>
      <rect x="3" y="4" width="18" height="13" rx="1" />
      <path d="M8 21h8M12 17v4" />
    </>
  ),
  folder: <path d="M3 6h6l2 2h10v11H3z" />,
  grid: (
    <>
      <rect x="3" y="3" width="7" height="7" />
      <rect x="14" y="3" width="7" height="7" />
      <rect x="3" y="14" width="7" height="7" />
      <rect x="14" y="14" width="7" height="7" />
    </>
  ),
  help: (
    <>
      <circle cx="12" cy="12" r="9" />
      <path d="M9.6 9a2.6 2.6 0 1 1 3.4 2.5c-.8.3-1 1-1 1.8M12 17h.01" />
    </>
  ),
  image: (
    <>
      <rect x="3" y="4" width="18" height="16" rx="1" />
      <circle cx="8.5" cy="9" r="1.5" />
      <path d="m4 17 5-5 4 4 2-2 5 5" />
    </>
  ),
  key: <path d="M14 8a4 4 0 1 1-1.2 2.8L21 3v4h-3v3h-3z" />,
  link: (
    <>
      <path d="M10 13a5 5 0 0 0 7.5.5l2-2a5 5 0 0 0-7-7l-1 1" />
      <path d="M14 11a5 5 0 0 0-7.5-.5l-2 2a5 5 0 0 0 7 7l1-1" />
    </>
  ),
  library: (
    <>
      <rect x="3" y="4" width="18" height="16" rx="1" />
      <circle cx="8.5" cy="9" r="1.5" />
      <path d="m4 17 5-5 4 4 2-2 5 5" />
    </>
  ),
  list: (
    <>
      <path d="M9 6h12M9 12h12M9 18h12" />
      <path d="M4 6h.01M4 12h.01M4 18h.01" />
    </>
  ),
  mods: (
    <>
      <path d="m4 7 8-4 8 4v10l-8 4-8-4z" />
      <path d="m4 7 8 4 8-4M12 11v10" />
    </>
  ),
  play: <path d="m8 5 11 7-11 7z" />,
  refresh: <path d="M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7" />,
  search: (
    <>
      <circle cx="11" cy="11" r="7" />
      <path d="m20 20-4-4" />
    </>
  ),
  settings: (
    <>
      <circle cx="12" cy="12" r="3" />
      <path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1-2.8 2.8-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.5V21h-4v-.1a1.7 1.7 0 0 0-1-1.5 1.7 1.7 0 0 0-1.9.3l-.1.1L4.2 17l.1-.1a1.7 1.7 0 0 0 .3-1.9A1.7 1.7 0 0 0 3.1 14H3v-4h.1a1.7 1.7 0 0 0 1.5-1 1.7 1.7 0 0 0-.3-1.9L4.2 7 7 4.2l.1.1a1.7 1.7 0 0 0 1.9.3 1.7 1.7 0 0 0 1-1.5V3h4v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.9-.3l.1-.1L19.8 7l-.1.1a1.7 1.7 0 0 0-.3 1.9 1.7 1.7 0 0 0 1.5 1h.1v4h-.1a1.7 1.7 0 0 0-1.5 1z" />
    </>
  ),
  audio: (
    <>
      <path d="M5 10v4h3l5 4V6L8 10z" />
      <path d="M17 9a4 4 0 0 1 0 6M19 6a8 8 0 0 1 0 12" />
    </>
  ),
  terminal: (
    <>
      <rect x="3" y="4" width="18" height="16" rx="1" />
      <path d="m7 9 3 3-3 3M13 15h4" />
    </>
  ),
  tools: (
    <>
      <rect x="3" y="4" width="18" height="16" rx="1" />
      <path d="m7 9 3 3-3 3M13 15h4" />
    </>
  ),
  warning: (
    <>
      <path d="M12 3 2.5 20h19z" />
      <path d="M12 9v5M12 17h.01" />
    </>
  ),
};

export function CemuIcon({
  name,
  className,
  ...props
}: { name: CemuIconName } & SVGProps<SVGSVGElement>) {
  return (
    <svg
      className={`cemu-icon${className ? ` ${className}` : ""}`}
      viewBox="0 0 24 24"
      aria-hidden="true"
      {...props}
    >
      {paths[name]}
    </svg>
  );
}
