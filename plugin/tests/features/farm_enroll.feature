Feature: A new device pairs with a one-time code and the coordinator's fingerprint

  Scenario: The right code and fingerprint give the device a working certificate
    Given a farm with a coordinator
    And an invitation for the worker "win4060"
    When the device enrols with the right code and the right fingerprint
    Then the device holds a certificate that the coordinator accepts as "worker" "win4060"
    And the invitation is used up

  Scenario: A wrong code is refused and costs an attempt
    Given a farm with a coordinator
    And an invitation for the worker "win4060"
    When the device enrols with a wrong code
    Then enrolment fails with "code was not accepted"
    And the invitation has 1 attempt left

  Scenario: Running out of attempts kills the invitation
    Given a farm with a coordinator
    And an invitation for the worker "win4060"
    When the device enrols with a wrong code twice
    Then the invitation is no longer active
    And enrolling with the right code now fails

  Scenario: A wrong fingerprint stops the device before it sends anything
    Given a farm with a coordinator
    And an invitation for the worker "win4060"
    When the device enrols with the right code and a wrong fingerprint
    Then enrolment fails with "not the one you gave"
    And the invitation has 2 attempts left

  Scenario: An invitation works once
    Given a farm with a coordinator
    And an invitation for the worker "win4060"
    When the device enrols with the right code and the right fingerprint
    And another device tries the same code
    Then enrolment fails with "no invitation"

  Scenario: An expired invitation is refused
    Given a farm with a coordinator
    And an invitation for the worker "win4060" that has expired
    When the device enrols with the right code and the right fingerprint
    Then enrolment fails with "no invitation"

  Scenario: The endpoint closes when the invitation is used up
    Given a farm with a coordinator
    And an invitation for the worker "win4060"
    When the enrolment endpoint runs and the device enrols
    Then the endpoint has stopped listening
