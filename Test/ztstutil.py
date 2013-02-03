class Str(object):
    def __init__(self):
        self.i=0
    def __str__(self):
        self.i+=1
        return str(self.i)

class CStr(Str):
    def __call__(self, s):
        self.i-=int(s)

class NBase(float):
    __slots__=("i",)
    def __init__(self):
        self.i=float(1)

class Int(NBase):
    def __int__(self):
        self.i*=4
        return int(self.i)

class CInt(Int):
    def __call__(self, i):
        self.i/=i

class Float(NBase):
    def __float__(self):
        self.i*=2
        return float(self.i)

class CFloat(Float):
    def __call__(self, i):
        self.i/=(i+1)

class Array(object):
    def __init__(self):
        self.accesses=[]

    def __len__(self):
        self.accesses+=['len:'+str(len(self.accesses)+1)]
        return len(self.accesses)

    def __getitem__(self, i):
        self.accesses+=['get:'+str(i)]
        return self.accesses[i]

class CArray(Array):
    def __call__(self, a):
        self.accesses+=['set:'+'|'.join(a)]

class Hash(object):
    def accappend(self, a):
        if self.acc and self.acc[-1][0] == a:
            self.acc[-1][1]+=1
        else:
            self.acc.append([a, 1])

    def __init__(self):
        self.d = {'a': 'b'}
        self.acc = []

    def keys(self):
        self.accappend('k')
        return self.d.keys()

    def __getitem__(self, key):
        self.accappend('['+key+']')
        if key == 'acc':
            return ';'.join([k[0]+('*'+str(k[1]) if k[1]>1 else '') for k in self.acc])
        return self.d.get(key)

    def __delitem__(self, key):
        self.accappend('!['+key+']')
        self.d.pop(key)

    def __contains__(self, key):
        # Will be used only if I switch from PyMapping_HasKey to
        # PySequence_Contains
        self.accappend('?['+key+']')
        return key == 'acc' or key in self.d

    def __setitem__(self, key, val):
        self.accappend('['+key+']='+val)
        self.d[key] = val

    def __iter__(self):
        self.accappend('i')
        return iter(['acc']+self.d.keys())

class EHash(object):
    def __getitem__(self, i):
        raise SystemError()

    # Invalid number of arguments
    def __setitem__(self, i):
        pass

    def __iter__(self):
        raise NotImplementedError()

    def keys(self):
        raise ValueError()

class EStr(object):
    def __str__(self):
        raise NotImplementedError()

    def __call__(self, s):
        raise KeyError()

class EHash2(object):
    def __getitem__(self, i):
        return EStr()

    def __iter__(self):
        return iter(range(1))

class EHash3(object):
    def __getitem__(self, i):
        return None

    def __iter__(self):
        return iter([EStr()])

class ENum(float):
    def __int__(self):
        raise IndexError()

    def __float__(self):
        raise KeyError()

    def __call__(self, i):
        raise ValueError()

class EArray(Array):
    def __len__(self):
        raise NotImplementedError()

    def __call__(self, a):
        raise IndexError()
