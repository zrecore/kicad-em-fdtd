Feature: The coordinator authorises every request against the policy file

  Scenario: Unknown and unverified devices are refused
    Given an empty policy
    Then "worker" "w1" may not "run"
    And no identity may "status"

  Scenario: A worker gets only the run grant by default
    Given an empty policy
    When the worker "w1" is enrolled
    Then "worker" "w1" may "run"
    And "worker" "w1" may not "submit"
    And "worker" "w1" may not "cancel"

  Scenario: A client may submit but not run
    Given an empty policy
    When the client "mac" is enrolled
    Then "client" "mac" may "submit"
    And "client" "mac" may not "run"

  Scenario: Removing a device takes the grants away at once
    Given an empty policy
    When the worker "w1" is enrolled
    And the worker "w1" is removed
    Then "worker" "w1" may not "run"

  Scenario: An unknown action is refused even for an enrolled device
    Given an empty policy
    When the worker "w1" is enrolled
    Then "worker" "w1" may not "format_disk"

  Scenario: Limits can be tightened per device and the defaults apply otherwise
    Given an empty policy
    When the worker "w1" is enrolled with the limit max_steps 5000
    Then the limit max_steps of "worker" "w1" is 5000
    And the limit max_input_bytes of "worker" "w1" is 67108864
