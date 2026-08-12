OrderedCollection := SequenceableCollection [

    | contents firstIndex lastIndex |

    class new [
        ^self new: 16
    ]

    class new: capacity [
        ^super new initialize: capacity
    ]

    class with: anObject [
        ^self new
            add: anObject;
            yourself
    ]

    class withAll: aCollection [
        ^(self new: aCollection size)
            addAll: aCollection;
            yourself
    ]

    , aCollection [
        | result |
        result := self class new: self size + aCollection size.
        result addAll: self.
        result addAll: aCollection.
        ^result
    ]

    initialize: requestedCapacity [
        | capacity |
        requestedCapacity < 0 ifTrue: [
            ^InvalidArgumentError signal
        ].
        capacity := requestedCapacity max: 1.
        contents := Array new: capacity.
        firstIndex := 1.
        lastIndex := 0.
        ^self
    ]

    size [
        ^lastIndex - firstIndex + 1
    ]

    at: index [
        | physicalIndex |
        physicalIndex := firstIndex + index - 1.
        (index < 1 or: [physicalIndex > lastIndex]) ifTrue: [
            ^OutOfRangeError signal
        ].
        ^contents at: physicalIndex
    ]

    at: index put: anObject [
        | physicalIndex |
        physicalIndex := firstIndex + index - 1.
        (index < 1 or: [physicalIndex > lastIndex]) ifTrue: [
            ^OutOfRangeError signal
        ].
        ^contents at: physicalIndex put: anObject
    ]

    first [
        self isEmpty ifTrue: [^EmptyCollectionError signal].
        ^contents at: firstIndex
    ]

    last [
        self isEmpty ifTrue: [^EmptyCollectionError signal].
        ^contents at: lastIndex
    ]

    add: anObject [
        ^self addLast: anObject
    ]

    addFirst: anObject [
        self isEmpty ifTrue: [
            firstIndex := contents size // 2 + 1.
            lastIndex := firstIndex.
            ^contents at: firstIndex put: anObject
        ].
        firstIndex = 1 ifTrue: [
            self growAndCenter
        ].
        firstIndex := firstIndex - 1.
        ^contents at: firstIndex put: anObject
    ]

    addLast: anObject [
        lastIndex = contents size ifTrue: [
            self makeRoomAtLast
        ].
        lastIndex := lastIndex + 1.
        ^contents at: lastIndex put: anObject
    ]

    removeFirst [
        | result |
        self isEmpty ifTrue: [^EmptyCollectionError signal].
        result := contents at: firstIndex.
        contents at: firstIndex put: nil.
        firstIndex := firstIndex + 1.
        self resetIndicesIfEmpty.
        ^result
    ]

    removeLast [
        | result |
        self isEmpty ifTrue: [^EmptyCollectionError signal].
        result := contents at: lastIndex.
        contents at: lastIndex put: nil.
        lastIndex := lastIndex - 1.
        self resetIndicesIfEmpty.
        ^result
    ]

    remove: anObject ifAbsent: absentBlock [
        | index |
        index := self
            indexOf: anObject
            ifAbsent: [^absentBlock value].
        ^self removeAt: index
    ]

    removeAt: index [
        | result physicalIndex |
        result := self at: index.
        physicalIndex := firstIndex + index - 1.
        [physicalIndex < lastIndex] whileTrue: [
            contents
                at: physicalIndex
                put: (contents at: physicalIndex + 1).
            physicalIndex := physicalIndex + 1
        ].
        contents at: lastIndex put: nil.
        lastIndex := lastIndex - 1.
        self resetIndicesIfEmpty.
        ^result
    ]

    do: aBlock [
        | index |
        index := firstIndex.
        [index <= lastIndex] whileTrue: [
            aBlock value: (contents at: index).
            index := index + 1
        ].
        ^self
    ]

    makeRoomAtLast [
        firstIndex > 1
            ifTrue: [self compactToFront]
            ifFalse: [self growAndCenter]
    ]

    compactToFront [
        | count offset |
        count := self size.
        offset := 0.
        [offset < count] whileTrue: [
            contents
                at: offset + 1
                put: (contents at: firstIndex + offset).
            offset := offset + 1
        ].
        [offset < contents size] whileTrue: [
            contents at: offset + 1 put: nil.
            offset := offset + 1
        ].
        firstIndex := 1.
        lastIndex := count
    ]

    growAndCenter [
        | oldContents oldFirst count newCapacity newFirst offset |
        oldContents := contents.
        oldFirst := firstIndex.
        count := self size.
        newCapacity := (oldContents size * 2) max: 8.
        contents := Array new: newCapacity.
        newFirst := (newCapacity - count) // 2 + 1.
        offset := 0.
        [offset < count] whileTrue: [
            contents
                at: newFirst + offset
                put: (oldContents at: oldFirst + offset).
            offset := offset + 1
        ].
        firstIndex := newFirst.
        lastIndex := newFirst + count - 1
    ]

    resetIndicesIfEmpty [
        lastIndex < firstIndex ifTrue: [
            firstIndex := 1.
            lastIndex := 0
        ]
    ]
]
