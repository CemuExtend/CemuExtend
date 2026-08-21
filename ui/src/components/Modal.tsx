import { useEffect, useRef, type ReactNode } from "react";

export function Modal({ title, children, onClose }: { title: string; children: ReactNode; onClose: () => void }) {
  const dialog = useRef<HTMLDialogElement>(null);
  useEffect(() => { dialog.current?.showModal(); }, []);
  return <dialog ref={dialog} onCancel={(event) => { event.preventDefault(); onClose(); }} aria-labelledby="modal-title">
    <header><h2 id="modal-title">{title}</h2><button className="icon-button" aria-label="Close" onClick={onClose}>×</button></header>
    {children}
  </dialog>;
}
