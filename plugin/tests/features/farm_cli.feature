Feature: The farm command line switches roles on only when the user confirms

  Scenario: Enabling asks for a typed confirmation and a wrong answer enables nothing
    Given a clean farm home
    When the user runs "enable coordinator" and answers "yes"
    Then the command fails
    And the coordinator is off

  Scenario: The typed confirmation enables the role and says what it allows
    Given a clean farm home
    When the user runs "enable coordinator" and answers "enable coordinator"
    Then the command succeeds and the output mentions "certificate authority"
    And the coordinator is on and the worker is off

  Scenario: The --yes flag skips the question
    Given a clean farm home
    When the user runs "enable worker --yes" and answers "nothing"
    Then the worker is on

  Scenario: Disable switches it off again and status shows both
    Given a clean farm home
    When the user runs "enable worker --yes" and answers "nothing"
    And the user runs "disable worker" and answers "nothing"
    And the user runs "status --json" and answers "nothing"
    Then the status says every role is off

  Scenario: A Python without TLS 1.3 cannot enable the farm
    Given a clean farm home
    And the Python has no TLS 1.3
    When the user runs "enable coordinator --yes" and answers "nothing"
    Then the command fails
    And the coordinator is off

  Scenario: The worker command refuses to start without a GPU turn mechanism
    Given a clean farm home
    When the user runs "enable worker --yes" and answers "nothing"
    Then starting the worker with only a solver and a device exits with the turn message
