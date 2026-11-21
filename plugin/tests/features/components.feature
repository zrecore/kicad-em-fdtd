Feature: Footprints are exported with a body outline
  The solver can model a part's body, so each footprint carries its bounding rectangle.

  Scenario: A footprint's bounding box becomes a body polygon in metres
    Given a footprint U1 at 5 mm, 5 mm whose bounding box is 3 mm wide and 2 mm tall starting at 3.5 mm, 4 mm
    When the components are read
    Then U1 has a body polygon with corners 0.0035,0.004 and 0.0065,0.006 in metres
    And the notes say that heights are not available from KiCAD

  Scenario: A board API that cannot give a footprint box still exports the footprint
    Given a footprint U2 whose bounding box cannot be read
    When the components are read
    Then U2 is exported without a body

  Scenario: Pads and the do-not-populate flag are exported with each footprint
    Given a footprint R5 marked do not populate with pads 1 on net FEED at 2 mm, 3 mm and 2 on net ANT at 3 mm, 3 mm
    When the components are read with their pads
    Then R5 is marked dnp and surface-mounted
    And R5 has 2 pads numbered 1 and 2 on nets FEED and ANT, with pad 1 at 0.002,0.003 metres
    And a pad belongs to the footprint it sits in, not to another one
