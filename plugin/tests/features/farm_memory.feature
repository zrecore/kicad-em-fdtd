Feature: A job goes only to a GPU that has the memory for it
  The solver's own dry run says how much device memory a job needs; a job may use 80 percent of a GPU's memory,
  the solver's rule. A device that did not say how much memory it has is not refused.

  Scenario: A job too big for the small GPU goes to the big one even when the small one is faster
    Given a scheduler with a small fast GPU of 4 GiB and a big slow GPU of 16 GiB
    When a job that needs 5 GiB of GPU memory is submitted and its input arrives
    Then the job is offered to the big GPU

  Scenario: A job that fits both GPUs still goes to the fastest one
    Given a scheduler with a small fast GPU of 4 GiB and a big slow GPU of 16 GiB
    When a job that needs 1 GiB of GPU memory is submitted and its input arrives
    Then the job is offered to the small fast GPU

  Scenario: A job too big for every GPU waits and says why
    Given a scheduler with a small fast GPU of 4 GiB and a big slow GPU of 16 GiB
    When a job that needs 20 GiB of GPU memory is submitted and its input arrives
    Then the job is not offered and its reason names the memory and the largest GPU

  Scenario: A job pinned to a GPU that is too small says so instead of waiting for it
    Given a scheduler with a small fast GPU of 4 GiB and a big slow GPU of 16 GiB
    When a job pinned to the small GPU that needs 5 GiB is submitted and its input arrives
    Then the job is not offered and its reason names the memory and the small GPU

  Scenario: A GPU that did not say how much memory it has is not refused
    Given a scheduler with one GPU that reports no memory
    When a job that needs 20 GiB of GPU memory is submitted and its input arrives
    Then the job is offered to the small fast GPU
