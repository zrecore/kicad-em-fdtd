Feature: The request for field data travels to the solver and nowhere else
  Scenario: The flag reaches the solver's command line
    Given a job that asks to keep its fields
    When the worker builds the solver's command line
    Then it contains --keep-fields

  Scenario: A job without the request has the command line it always had
    Given a job that does not ask to keep its fields
    When the worker builds the solver's command line
    Then it does not contain --keep-fields
    And the validated job has no keep_fields entry

  Scenario: Only true or false is a request
    Given a job whose keep_fields is the text "yes"
    Then validating it is refused

  Scenario: A job that asks for its fields runs without checkpoints
    Given an offer with checkpointing for a job that asks to keep its fields
    Then the worker plans no checkpoint
    And an offer with checkpointing for a plain job gets a checkpoint plan
