MethodDictionary := Object [

    <shape: indexedValues>
    <abstract: true>

    class new [
        ^self shouldNotImplement
    ]

    class new: size [
        ^self shouldNotImplement
    ]

    class basicNew [
        ^self shouldNotImplement
    ]

    class basicNew: size [
        ^self shouldNotImplement
    ]

    at: aSelector [
        ^self
            at: aSelector
            ifAbsent: [NotFoundError signal]
    ]

    at: aSelector ifAbsent: absentBlock [
        | index |
        index := 1.
        [index <= self size] whileTrue: [
            (super at: index) == aSelector ifTrue: [
                ^super at: index + 1
            ].
            index := index + 2
        ].
        ^absentBlock value
    ]

    includesKey: aSelector [
        ^(self
            at: aSelector
            ifAbsent: [^false]) notNil
    ]

    keysAndValuesDo: aBlock [
        | index |
        index := 1.
        [index <= self size] whileTrue: [
            aBlock
                value: (super at: index)
                value: (super at: index + 1).
            index := index + 2
        ].
        ^self
    ]

    at: aSelector put: aCompiledMethod [
        ^self shouldNotImplement
    ]
]
