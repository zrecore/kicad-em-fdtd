Feature: A device's speed comes from whole runs only

  A resumed run's final speed line counts only its last segment, and a very short run is dominated by
  setup, so neither tells how fast the device is. Jobs that ran at normal speed once read 417, 636 and
  848 Mcells/s and made the listing look like a slowdown.

  Background:
    Given a worker whose RTX has a recorded speed of 1700 Mcells/s

  Scenario: The last line of a resumed run does not change the speed
    When a resumed run reports 417 Mcells/s after 4467 seconds
    Then the recorded speed is within 0.5 percent of 1700 Mcells/s

  Scenario: A short run does not change the speed
    When a run reports 600 Mcells/s after 40 seconds
    Then the recorded speed is within 0.5 percent of 1700 Mcells/s

  Scenario: Full runs move the speed to their recent median, and one slow run does not drag it
    When a run reports 1750 Mcells/s after 1900 seconds
    And a run reports 1800 Mcells/s after 1900 seconds
    And a run reports 1650 Mcells/s after 1900 seconds
    Then the recorded speed is within 1 percent of 1725 Mcells/s
    When a run reports 1000 Mcells/s after 1900 seconds
    Then the recorded speed is within 1 percent of 1700 Mcells/s

  Scenario: A device with no speed yet takes the rate of a short run, so it has some figure
    Given a worker with no recorded speed
    When a run reports 1185 Mcells/s after 40 seconds
    Then the recorded speed is within 0.5 percent of 1185 Mcells/s

  Scenario: A speed file written by an older version still loads
    Given a speed file in the old format with 1900 Mcells/s for the RTX
    Then the recorded speed is within 0.5 percent of 1900 Mcells/s
