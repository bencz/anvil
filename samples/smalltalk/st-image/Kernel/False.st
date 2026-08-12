False := Boolean [

    not [ ^true ]
    & aBoolean [ ^self ]
    | aBoolean [ ^aBoolean ]
    ifTrue: aBlock [ ^nil ]
    ifFalse: aBlock [ ^aBlock value ]
    ifTrue: trueBlock ifFalse: falseBlock [ ^falseBlock value ]
    and: aBlock [ ^self ]
    or: aBlock [ ^aBlock value ]
]
