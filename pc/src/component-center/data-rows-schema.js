const need=q=>{if(!q)throw new TypeError('Invalid bounded data rows declaration');};
function validWifiEndpoint(url) {
  if (typeof url !== 'string' || !url.startsWith('https://')) return false;
  const target = url.slice(8);
  const delimiter = target.search(/[/?]/);
  const host = delimiter < 0 ? target : target.slice(0, delimiter);
  const suffix = delimiter < 0 ? '' : target.slice(delimiter);
  const path = suffix.startsWith('/') ? suffix : '/' + suffix;
  const labels = host.split('.');
  return host.length <= 127 && labels.length >= 2 && /[a-z]/i.test(labels.at(-1))
    && labels.every(label => /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/i.test(label))
    && path.length <= 255 && !path.startsWith('//')
    && [...path].every(char => char >= '!' && char <= '~' && char !== '#' && char.charCodeAt(0) !== 92);
}

function validWifiFeedMap(map) {
  return map && typeof map === 'object' && !Array.isArray(map)
    && ['rows', 'id', 'label'].every(key => Object.hasOwn(map, key))
    && Object.entries(map).every(([key, path]) => {
      if (!['rows', 'id', 'label', 'value', 'detail', 'meta', 'tone'].includes(key)
          || typeof path !== 'string' || path.length > 63) return false;
      if (key === 'rows' && path === '$') return true;
      const segments = path.split('.');
      return segments.length <= 4 && segments.every(segment => segment && !/[^A-Za-z0-9_-]/.test(segment));
    });
}

function validWifiFeedHttp(http) {
  if (!http || typeof http !== 'object' || Array.isArray(http)) return false;
  const fields = Object.entries(http);
  return fields.length > 0 && fields.every(([key, value]) => {
    if (key === 'accept') {
      return ['application/json', 'application/geo+json', 'application/hal+json', 'application/vnd.api+json'].includes(value);
    }
    if (key === 'revalidate') return value === true;
    return key === 'cache' && value === 'last-good';
  });
}

export function validateDataRows(r){
 const d=r.data,s=r.surface||{},nodes=s.elements||[];
 if(d!==undefined){
  need(d&&typeof d==='object'&&!Array.isArray(d)&&Object.hasOwn(d,'source')&&Object.hasOwn(d,'page_var')&&Object.keys(d).every(k=>['source','page_var','select_var','filters','refresh_action','url','response_map','http'].includes(k)));
  need(d.source === 'wifi.feed' ? validWifiEndpoint(d.url) : !Object.hasOwn(d, 'url') && !String(d.source).startsWith('wifi.feed.'));
  if (Object.hasOwn(d, 'response_map')) need(d.source === 'wifi.feed' && validWifiFeedMap(d.response_map));
  if (Object.hasOwn(d, 'http')) need(d.source === 'wifi.feed' && validWifiFeedHttp(d.http));
  const int=k=>need(typeof k==='string'&&/^[A-Za-z0-9_.-]{1,31}$/.test(k)&&k!==d.page_var&&r.vars?.[k]?.type==='int');
  if(Object.hasOwn(d,'select_var'))int(d.select_var);
  if(Object.hasOwn(d,'refresh_action'))need(d.refresh_action==='data.refresh'&&r.transitions?.some(t=>t.on===d.refresh_action));
  if(Object.hasOwn(d,'filters')){need(Array.isArray(d.filters)&&d.filters.length>=1&&d.filters.length<=4);const seen=new Set();
   for(const f of d.filters){need(f&&typeof f==='object'&&Object.keys(f).length===3&&['var','field','values'].every(k=>Object.hasOwn(f,k))&&!seen.has(f.var)&&['label','value','detail','meta','tone'].includes(f.field));int(f.var);seen.add(f.var);need(Array.isArray(f.values)&&f.values.length>=1&&f.values.length<=8);
    for(const v of f.values)need(f.field==='tone'&&Number.isInteger(v)&&v>=-1&&v<=1||typeof v==='string'&&new TextEncoder().encode(v).length>=1&&new TextEncoder().encode(v).length<=24&&!/[\u0000-\u001f\u007f]/u.test(v));
   }
  }
 }
 const declared=d&&['select_var','filters','refresh_action'].some(k=>Object.hasOwn(d,k))||s.data_view?.layout==='rows'||nodes.some(n=>Object.hasOwn(n,'row_bind'));
 if(declared&&s.data_view){const v=s.data_view;need(Object.keys(v).length===7&&['engine','x','y','w','h','layout','colors'].every(k=>Object.hasOwn(v,k))&&v.engine==='p4-data-surface-v1'&&['todos','metrics','rows'].includes(v.layout));}
 if(declared){need(d&&s.data_view&&typeof s.data_view==='object'&&s.bindings==='p4-variable-bindings-v1');for(const n of nodes)if(Object.hasOwn(n,'row_bind'))need(['label','value','detail','meta'].includes(n.row_bind)&&['text','button'].includes(n.type)&&!['text','bind','var_bind','field_bind','key_for','text_at'].some(k=>Object.hasOwn(n,k)));}
}
