import { useEffect, useRef, useState, type ReactNode } from "react";
import { CemuIcon, type CemuIconName } from "./CemuIcon";

export type MenuCommand = {
  label?: string;
  onSelect?: () => void;
  disabled?: boolean;
  checked?: boolean;
  separator?: boolean;
};

export type MenuGroup = {
  label: string;
  commands: MenuCommand[];
};

export function MenuBar({ groups }: { groups: MenuGroup[] }) {
  const [openIndex, setOpenIndex] = useState<number>();
  const root = useRef<HTMLElement>(null);

  useEffect(() => {
    const close = (event: PointerEvent) => {
      if (!root.current?.contains(event.target as Node))
        setOpenIndex(undefined);
    };
    const closeWithEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") setOpenIndex(undefined);
    };
    window.addEventListener("pointerdown", close);
    window.addEventListener("keydown", closeWithEscape);
    return () => {
      window.removeEventListener("pointerdown", close);
      window.removeEventListener("keydown", closeWithEscape);
    };
  }, []);

  return (
    <nav className="menubar" aria-label="Application menu" ref={root}>
      {groups.map((group, index) => (
        <div className="menu-group" key={group.label}>
          <button
            type="button"
            className="menu-trigger"
            aria-expanded={openIndex === index}
            aria-haspopup="menu"
            onClick={() =>
              setOpenIndex((current) => (current === index ? undefined : index))
            }
            onPointerEnter={() =>
              setOpenIndex((current) =>
                current === undefined ? undefined : index,
              )
            }
          >
            {group.label}
          </button>
          {openIndex === index && (
            <div className="menu-popup" role="menu">
              {group.commands.map((command, commandIndex) =>
                command.separator ? (
                  <hr key={`separator-${commandIndex}`} />
                ) : (
                  <button
                    type="button"
                    key={command.label}
                    role="menuitem"
                    disabled={command.disabled}
                    onClick={() => {
                      command.onSelect?.();
                      setOpenIndex(undefined);
                    }}
                  >
                    <span aria-hidden="true">{command.checked ? "✓" : ""}</span>
                    {command.label}
                  </button>
                ),
              )}
            </div>
          )}
        </div>
      ))}
    </nav>
  );
}

export function ToolButton({
  icon,
  active,
  children,
  onClick,
  disabled,
}: {
  icon: CemuIconName;
  active?: boolean;
  children: ReactNode;
  onClick?: () => void;
  disabled?: boolean;
}) {
  return (
    <button
      className="toolbtn"
      aria-label={typeof children === "string" ? children : undefined}
      aria-current={active ? "page" : undefined}
      onClick={onClick}
      disabled={disabled}
    >
      <CemuIcon name={icon} />
      <span>{children}</span>
    </button>
  );
}

export function StatusBar({
  left,
  right,
}: {
  left: ReactNode;
  right?: ReactNode;
}) {
  return (
    <footer className="statusbar">
      <span>{left}</span>
      <span>{right}</span>
    </footer>
  );
}
