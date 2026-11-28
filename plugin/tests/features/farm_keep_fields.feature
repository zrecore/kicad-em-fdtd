Feature: A job brings back its field data only when asked
  The field data (3D view, slice, current, power flow, far field) is tens to hundreds of megabytes. A job
  returns it only when the submitter asks for it with keep_fields; nothing else changes for a job that does not.

  Background:
    Given a farm with a coordinator
    And a worker "gpu-box" offering "stub gpu a"
    And a client "alice"

  Scenario: A job without the request returns exactly what it always did
    When "alice" submits a job with the setting mesh_preset "medium"
    Then the job finishes
    And the results hold s11.s1p, summary.txt and settings.json and the run log

  Scenario: A job that asks for its fields gets them back in a folder that is a saved run
    When "alice" submits a job that asks to keep its fields
    Then the job finishes
    And the fetched folder holds the run manifest, the board and the field files in fields/
    And the solver was asked for the fields

  Scenario: A job whose field data would not fit the result limit is refused at submission
    When "alice" submits a job that asks to keep 300 MB of fields
    Then the submission is refused with the size and the result limit in the reason

  Scenario: A job whose real result is over the limit still delivers the S11 and says so
    Given the worker "gpu-box" has a result limit of 100000 bytes
    When "alice" submits a job that asks to keep fields and the solver writes 200000 bytes of them
    Then the job finishes
    And the results hold only the plain files and summary.txt says the fields were not kept
