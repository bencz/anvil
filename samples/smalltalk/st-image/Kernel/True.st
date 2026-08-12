True := Boolean [

    not [ ^false ]
    & aBoolean [ ^aBoolean ]
    | aBoolean [ ^self ]
    ifTrue: aBlock [ ^aBlock value ]
    ifFalse: aBlock [ ^nil ]
    ifTrue: trueBlock ifFalse: falseBlock [ ^trueBlock value ]
    and: aBlock [ ^aBlock value ]
    or: aBlock [ ^self ]
]
