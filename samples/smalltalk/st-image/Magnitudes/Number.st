Number := Magnitude [
    <abstract: true>

    + aNumber [
        ^self subclassResponsibility
    ]

    - aNumber [
        ^self subclassResponsibility
    ]

    * aNumber [
        ^self subclassResponsibility
    ]

    / aNumber [
        ^self subclassResponsibility
    ]

    negated [
        ^self subclassResponsibility
    ]

    abs [
        self < 0 ifTrue: [^self negated].
        ^self
    ]

    asFloat [
        ^self subclassResponsibility
    ]

    truncated [
        ^self subclassResponsibility
    ]

    floor [
        ^self subclassResponsibility
    ]

    ceiling [
        ^self subclassResponsibility
    ]

    rounded [
        ^self subclassResponsibility
    ]

    isNumber [
        ^true
    ]
]
