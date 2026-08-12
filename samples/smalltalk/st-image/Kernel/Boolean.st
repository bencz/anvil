Boolean := Object [

    not [
        ^self subclassResponsibility
    ]

    & aBoolean [
        ^self subclassResponsibility
    ]

    | aBoolean [
        ^self subclassResponsibility
    ]

    xor: aBoolean [
        ^(self == aBoolean) not
    ]

    ifTrue: aBlock [
        ^self subclassResponsibility
    ]

    ifFalse: aBlock [
        ^self subclassResponsibility
    ]

    ifTrue: trueBlock ifFalse: falseBlock [
        ^self subclassResponsibility
    ]

    and: aBlock [
        ^self subclassResponsibility
    ]

    or: aBlock [
        ^self subclassResponsibility
    ]
]
