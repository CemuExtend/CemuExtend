export type UpdateJobEvent = { type: string; payload: Record<string, unknown> };

export function routeUpdateJobEvent(
  pending: Map<string, UpdateJobEvent[]>,
  activeJob: string | undefined,
  incomingJob: string,
  event: UpdateJobEvent,
  apply: (event: UpdateJobEvent) => void,
): void {
  if (incomingJob === activeJob) apply(event);
  else {
    if (!pending.has(incomingJob) && pending.size >= 4)
      pending.delete(pending.keys().next().value as string);
    pending.set(incomingJob, [
      ...(pending.get(incomingJob) ?? []).slice(-15),
      event,
    ]);
  }
}

export function activateUpdateJob(
  pending: Map<string, UpdateJobEvent[]>,
  jobId: string,
  apply: (event: UpdateJobEvent) => void,
): void {
  for (const event of pending.get(jobId) ?? []) apply(event);
  pending.delete(jobId);
}
