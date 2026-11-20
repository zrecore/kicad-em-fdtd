Feature: Board coordinates are converted to SI metres
  The solver works in metres; KiCAD reports integer nanometres and datasheets use millimetres.

  Scenario: Nanometres from KiCAD become metres
    Given a length of 1600000 nanometres reported by KiCAD
    When it is converted to metres
    Then the result is 0.0016 metres

  Scenario: Millimetres from a datasheet become metres
    Given a length of 4.25 millimetres from a datasheet
    When it is converted to metres
    Then the result is 0.00425 metres
