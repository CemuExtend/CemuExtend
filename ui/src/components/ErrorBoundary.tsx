import { Component, type ErrorInfo, type ReactNode } from "react";

type State = { error?: Error };

export class ErrorBoundary extends Component<{ children: ReactNode }, State> {
  state: State = {};
  static getDerivedStateFromError(error: Error): State { return { error }; }
  componentDidCatch(error: Error, info: ErrorInfo): void { console.error(error, info.componentStack); }
  render(): ReactNode {
    if (!this.state.error) return this.props.children;
    return <main className="fatal" role="alert"><h1>CemuExtend UI stopped</h1><p>{this.state.error.message}</p><button onClick={() => location.reload()}>Reload UI</button></main>;
  }
}
