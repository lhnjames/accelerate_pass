# Action decision

Choose the next compiler-optimization action from the tools and evidence in
the task. Respect the action schema exactly. Prefer an evidence-backed change
that tests one clear hypothesis; do not repeat a failed action unless new
compiler or runtime evidence justifies it. Preserve program semantics and the
specified baseline. Return strict JSON only, with no markdown fences.
