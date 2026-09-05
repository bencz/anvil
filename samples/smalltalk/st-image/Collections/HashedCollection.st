HashedCollection := Collection [

    <abstract: true>

    | contents tally |

    class new [
        ^self new: 16
    ]

    class new: requestedCapacity [
        ^super new initialize: requestedCapacity
    ]

    initialize: requestedCapacity [
        | capacity required |
        requestedCapacity < 0 ifTrue: [
            ^InvalidArgumentError signal
        ].
        capacity := 8.
        required := requestedCapacity * 4.
        [capacity * 3 < required] whileTrue: [
            capacity := capacity * 2
        ].
        contents := Array new: capacity.
        tally := 0.
        ^self
    ]

    size [
        ^tally
    ]

    addAtEmptyIndex: index value: anObject [
        contents at: index put: anObject.
        tally := tally + 1.
        (tally * 4) >= (contents size * 3) ifTrue: [
            self grow
        ].
        ^anObject
    ]

    grow [
        | oldContents index item |
        oldContents := contents.
        contents := Array new: oldContents size * 2.
        tally := 0.
        index := 1.
        [index <= oldContents size] whileTrue: [
            item := oldContents at: index.
            item notNil ifTrue: [self reinsert: item].
            index := index + 1
        ]
    ]

    remove: anObject ifAbsent: absentBlock [
        | index |
        index := self
            indexOf: anObject
            ifAbsent: [^absentBlock value].
        self removeIndexAndRehashCluster: index.
        ^anObject
    ]

    removeIndexAndRehashCluster: removedIndex [
        | index item |
        contents at: removedIndex put: nil.
        tally := tally - 1.
        index := self indexAfter: removedIndex.
        [(item := contents at: index) notNil] whileTrue: [
            contents at: index put: nil.
            tally := tally - 1.
            self reinsert: item.
            index := self indexAfter: index
        ]
    ]

    indexAfter: index [
        index = contents size ifTrue: [^1].
        ^index + 1
    ]

    do: aBlock [
        | index item |
        index := 1.
        [index <= contents size] whileTrue: [
            item := contents at: index.
            item notNil ifTrue: [aBlock value: item].
            index := index + 1
        ].
        ^self
    ]

    indexOf: anObject ifAbsent: absentBlock [
        | index |
        index := self findIndex: anObject.
        (contents at: index) isNil ifTrue: [
            ^absentBlock value
        ].
        ^index
    ]

    findIndex: anObject [
        ^self subclassResponsibility
    ]

    reinsert: anObject [
        ^self subclassResponsibility
    ]
]
