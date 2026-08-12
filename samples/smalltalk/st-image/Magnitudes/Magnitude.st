Magnitude := Object [
    <abstract: true>

    < aMagnitude [
        ^self subclassResponsibility
    ]

    = aMagnitude [
        ^self subclassResponsibility
    ]

    > aMagnitude [
        ^aMagnitude < self
    ]

    <= aMagnitude [
        ^(self > aMagnitude) not
    ]

    >= aMagnitude [
        ^(self < aMagnitude) not
    ]

    between: lower and: upper [
        ^(lower <= self) and: [self <= upper]
    ]

    min: aMagnitude [
        self < aMagnitude ifTrue: [^self].
        ^aMagnitude
    ]

    max: aMagnitude [
        self > aMagnitude ifTrue: [^self].
        ^aMagnitude
    ]
]
