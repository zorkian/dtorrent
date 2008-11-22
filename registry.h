#ifndef REGISTRY_H
#define REGISTRY_H

#include <errno.h>

// registry.h:  Copyright 2008 Dennis Holmes  (dholmes@rahul.net)

template<class T> class Registry
{
 private:
  struct regnode{
    regnode *next;
    T registrant;
  } *m_registry;

 public:
  Registry(){ m_registry = (regnode *)0; }
  ~Registry();

  bool Register(T entity);
  bool Deregister(T entity);
  bool IsRegistered(T entity);
};


template<class T> Registry<T>::~Registry()
{
  regnode *p;

  while( m_registry ){
    p = m_registry;
    m_registry = m_registry->next;
    delete p;
  }
}


template<class T> bool Registry<T>::Register(T entity)
{
  regnode *p;

  if( (p = new regnode) ){
    p->registrant = entity;
    p->next = m_registry;
    m_registry = p;
  }else{
    errno = ENOMEM;
    return false;
  }
  return true;
}


template<class T> bool Registry<T>::Deregister(T entity)
{
  regnode *p, *pp = (regnode *)0;

  for( p = m_registry; p; pp = p, p = p->next ){
    if( p->registrant == entity ){
      if( pp ) pp->next = p->next;
      else m_registry = p->next;
      delete p;
      break;
    }
  }
  return m_registry ? false : true;
}


template<class T> bool Registry<T>::IsRegistered(T entity)
{
  regnode *p;

  for( p = m_registry; p; p = p->next ){
    if( p->registrant == entity ) break;
  }
  return p ? true : false;
}


#endif  // REGISTRY_H

