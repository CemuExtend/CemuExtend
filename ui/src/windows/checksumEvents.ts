export type BufferedJobEvent<T> = { type: string; payload: T };

export function routeJobEvent<T>(pending: Map<string, BufferedJobEvent<T>[]>, activeJob: string | undefined,
  incomingJob: string, event: BufferedJobEvent<T>, apply: (event: BufferedJobEvent<T>) => void): void {
  if (incomingJob === activeJob) apply(event);
  else pending.set(incomingJob, [...(pending.get(incomingJob) ?? []), event]);
}

export function activateJob<T>(pending: Map<string, BufferedJobEvent<T>[]>, jobId: string,
  apply: (event: BufferedJobEvent<T>) => void): void {
  const queued = pending.get(jobId) ?? [];
  pending.delete(jobId);
  queued.forEach(apply);
}
