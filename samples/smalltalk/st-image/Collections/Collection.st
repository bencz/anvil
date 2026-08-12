Collection := Object [
    <abstract: true>

    add: anObject [
        ^self subclassResponsibility
    ]

    do: aBlock [
        ^self subclassResponsibility
    ]

    size [
        | count |
        count := 0.
        self do: [ :each |
            count := count + 1
        ].
        ^count
    ]

    addAll: aCollection [
        aCollection do: [ :each |
            self add: each
        ].
        ^aCollection
    ]

    remove: anObject [
        ^self
            remove: anObject
            ifAbsent: [NotFoundError signal]
    ]

    remove: anObject ifAbsent: absentBlock [
        ^self subclassResponsibility
    ]

    includes: anObject [
        self do: [ :each |
            each = anObject ifTrue: [^true]
        ].
        ^false
    ]

    detect: predicate ifNone: absentBlock [
        self do: [ :each |
            (predicate value: each) ifTrue: [^each]
        ].
        ^absentBlock value
    ]

    collect: transform [
        | result |
        result := OrderedCollection new: self size.
        self do: [ :each |
            result add: (transform value: each)
        ].
        ^result
    ]

    select: predicate [
        | result |
        result := OrderedCollection new: self size.
        self do: [ :each |
            (predicate value: each) ifTrue: [
                result add: each
            ]
        ].
        ^result
    ]

    reject: predicate [
        ^self select: [ :each |
            (predicate value: each) not
        ]
    ]

    asArray [
        | result index |
        result := Array new: self size.
        index := 1.
        self do: [ :each |
            result at: index put: each.
            index := index + 1
        ].
        ^result
    ]

    asSet [
        ^Set withAll: self
    ]

    isEmpty [
        ^self size = 0
    ]

    notEmpty [
        ^self isEmpty not
    ]
]
