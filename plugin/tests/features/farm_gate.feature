Feature: The GPU farm is off until the user turns it on
  Nothing listens, dials or writes files unless a role was enabled by the user, on this machine only.

  Scenario: A fresh machine has every role off and importing the package does nothing
    Given a clean farm home
    When the farm package is imported with sockets forbidden
    Then no farm file exists
    And the coordinator is off and the worker is off

  Scenario: A role that is off refuses to run
    Given a clean farm home
    When the coordinator is required to run
    Then it is refused and the message says how to enable it

  Scenario: Enabling one role leaves the other off
    Given a clean farm home
    When the user enables the worker
    Then the worker is on and the coordinator is off

  Scenario: Disabling switches a role off again
    Given a clean farm home
    When the user enables the worker
    And the user disables the worker
    Then the worker is off

  Scenario: A damaged switch file means off
    Given a clean farm home
    And a switch file that is not JSON
    Then the coordinator is off and the worker is off

  Scenario: Only the exact value true turns a role on
    Given a clean farm home
    And a switch file that says the worker is "yes"
    Then the worker is off
