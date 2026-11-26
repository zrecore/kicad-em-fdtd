Feature: The client estimates how long a job will take before sending it

  Scenario: The estimate comes from the solver's own dry run and is scaled to what runs really take
    Given a solver whose dry run says 9456774 cells and an upper bound of 278043 steps
    When the client estimates a job with the default stop threshold
    Then the estimate has 9456774 cells and about 139000 steps

  Scenario Outline: A stricter stop threshold means more of the upper bound
    Given a solver whose dry run says 1000000 cells and an upper bound of 100000 steps
    When the client estimates a job with the stop threshold <threshold>
    Then the estimate has about <steps> steps

    Examples:
      | threshold | steps |
      | 0.01      | 50000 |
      | 0.001     | 50000 |
      | 0.0003    | 70000 |
      | 0.0001    | 100000 |
      | 0.00001   | 100000 |

  Scenario: A solver that cannot do a dry run gives no estimate and says why
    Given a solver that fails its dry run
    When the client estimates a job with the default stop threshold
    Then there is no estimate and the reason mentions "dry run"

  Scenario: The dry run is the CPU one and writes nothing next to the board
    Given a solver whose dry run says 1000 cells and an upper bound of 100 steps
    When the client estimates a job with the default stop threshold
    Then the dry run was called with "--cpu", "--dry-run" and "--json" and a project folder of its own
