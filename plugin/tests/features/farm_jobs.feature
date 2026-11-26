Feature: A job is validated data and the worker builds the command line

  Background:
    Given the solver's settings with tiers

  Scenario: A normal job becomes a command line with the farm's own flags
    When the job has profile "bluetooth", setting mesh_preset "medium", setting decay_threshold 0.001 and one refine box
    And the argv is built for device "99cbe8b6" in "/jobs/j1"
    Then the argv starts with the solver and the snapshot
    And it contains "--gpu" and "--device 99cbe8b6" and "--project-dir /jobs/j1"
    And it contains "--set mesh_preset=medium"

  Scenario Outline: A job that tries to reach outside is refused
    When the job is <attack>
    Then validation fails

    Examples:
      | attack                                       |
      | a setting named backend                      |
      | a setting that does not exist                |
      | an expert-tier setting                       |
      | a setting value that starts with a dash      |
      | a setting value with a shell metacharacter   |
      | a max_steps above the limit                  |
      | an unknown field named project_dir           |
      | an unknown field named argv                  |
      | a net name that starts with a dash           |
      | a net name containing a comma                |
      | a part reference with a slash                |
      | a body treatment that is not a known one     |
      | a refine box with four numbers               |
      | a non-finite number                          |
      | a boolean where a number is wanted           |

  Scenario Outline: A bad device selector never reaches the command line
    When a normal job is prepared
    And the argv is built for device "<device>" in "/jobs/j1"
    Then building fails

    Examples:
      | device     |
      | ; rm -rf   |
      | -1         |
      | 99cbe8b6 x |
      | (empty)    |

  Scenario: Odd but legal net names stay one argument each
    When the job has the net "/RF_OUT(+)" and the ground net "GND"
    And the argv is built for device "0" in "/jobs/j1"
    Then the argv has "--nets" followed by exactly "/RF_OUT(+)"

  Scenario: A sweep splits into single-point jobs, geometry stretches included
    When a sweep of "mesh_preset" over 3 values is expanded
    Then there are 3 jobs each with one value
    When a sweep of "geom.ANT.x" over 2 values is expanded
    Then each job carries the value as a geometry stretch
