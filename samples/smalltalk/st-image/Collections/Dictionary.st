Dictionary := HashedCollection [

    add: anAssociation [
        | index existing |
        (anAssociation isKindOf: Association) ifFalse: [
            ^InvalidArgumentError signal
        ].
        index := self findIndex: anAssociation key.
        existing := contents at: index.
        existing isNil
            ifTrue: [
                self addAtEmptyIndex: index value: anAssociation.
                ^anAssociation
            ]
            ifFalse: [
                existing value: anAssociation value.
                ^existing
            ]
    ]

    at: key [
        ^self
            at: key
            ifAbsent: [NotFoundError signal]
    ]

    at: key ifAbsent: absentBlock [
        | index association |
        index := self
            indexOf: key
            ifAbsent: [^absentBlock value].
        association := contents at: index.
        ^association value
    ]

    at: key ifPresent: presentBlock [
        | index association |
        index := self
            indexOf: key
            ifAbsent: [^nil].
        association := contents at: index.
        ^presentBlock value: association value
    ]

    at: key put: value [
        | index association |
        index := self findIndex: key.
        association := contents at: index.
        association isNil
            ifTrue: [
                association := Association key: key value: value.
                self addAtEmptyIndex: index value: association
            ]
            ifFalse: [association value: value].
        ^value
    ]

    at: key ifAbsentPut: valueBlock [
        ^self at: key ifAbsent: [
            | value |
            value := valueBlock value.
            self at: key put: value.
            value
        ]
    ]

    includesKey: key [
        | index |
        index := self findIndex: key.
        ^(contents at: index) notNil
    ]

    removeKey: key [
        ^self
            removeKey: key
            ifAbsent: [NotFoundError signal]
    ]

    removeKey: key ifAbsent: absentBlock [
        | index value |
        index := self
            indexOf: key
            ifAbsent: [^absentBlock value].
        value := (contents at: index) value.
        self removeIndexAndRehashCluster: index.
        ^value
    ]

    remove: anAssociation ifAbsent: absentBlock [
        (anAssociation isKindOf: Association) ifFalse: [
            ^absentBlock value
        ].
        self
            removeKey: anAssociation key
            ifAbsent: [^absentBlock value].
        ^anAssociation
    ]

    do: aBlock [
        ^self associationsDo: [ :association |
            aBlock value: association value
        ]
    ]

    associationsDo: aBlock [
        ^super do: aBlock
    ]

    keysDo: aBlock [
        ^self associationsDo: [ :association |
            aBlock value: association key
        ]
    ]

    keysAndValuesDo: aBlock [
        ^self associationsDo: [ :association |
            aBlock
                value: association key
                value: association value
        ]
    ]

    keys [
        | result |
        result := Set new: self size.
        self keysDo: [ :key |
            result add: key
        ].
        ^result
    ]

    values [
        | result |
        result := OrderedCollection new: self size.
        self do: [ :value |
            result add: value
        ].
        ^result
    ]

    associations [
        | result |
        result := OrderedCollection new: self size.
        self associationsDo: [ :association |
            result add: association
        ].
        ^result
    ]

    findIndex: key [
        | index association mask |
        mask := contents size - 1.
        index := (key hash bitAnd: mask) + 1.
        [true] whileTrue: [
            association := contents at: index.
            association isNil ifTrue: [^index].
            association key = key ifTrue: [^index].
            index := self indexAfter: index
        ]
    ]

    reinsert: association [
        | index |
        index := self findIndex: association key.
        contents at: index put: association.
        tally := tally + 1.
        ^association
    ]
]
