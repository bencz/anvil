Set := HashedCollection [

    class withAll: aCollection [
        ^(self new: aCollection size)
            addAll: aCollection;
            yourself
    ]

    add: anObject [
        | index |
        anObject isNil ifTrue: [^anObject].
        index := self findIndex: anObject.
        (contents at: index) isNil ifTrue: [
            self addAtEmptyIndex: index value: anObject
        ].
        ^anObject
    ]

    includes: anObject [
        | index |
        anObject isNil ifTrue: [^false].
        index := self findIndex: anObject.
        ^(contents at: index) notNil
    ]

    findIndex: anObject [
        | index item mask |
        mask := contents size - 1.
        index := (anObject hash bitAnd: mask) + 1.
        [true] whileTrue: [
            item := contents at: index.
            item isNil ifTrue: [^index].
            item = anObject ifTrue: [^index].
            index := self indexAfter: index
        ]
    ]

    reinsert: anObject [
        | index |
        index := self findIndex: anObject.
        contents at: index put: anObject.
        tally := tally + 1.
        ^anObject
    ]
]
