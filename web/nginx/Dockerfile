FROM node:18-alpine AS build

WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY tsconfig.client.json ./
COPY nginx/www/js/index.ts ./nginx/www/js/index.ts
RUN npm run build:client

FROM nginx:1.27-alpine

RUN rm -f /usr/share/nginx/html/index.html \
	&& cat > /etc/nginx/conf.d/default.conf <<'EOF'
server {
	listen       80;
	server_name  localhost;

	root   /usr/share/nginx/html;
	index  index.html;

	location / {
		try_files $uri $uri/ =404;
	}
}
EOF

COPY nginx/www/index.html /usr/share/nginx/html/index.html
COPY nginx/www/styles/ /usr/share/nginx/html/styles/
COPY --from=build /app/dist/client/index.js /usr/share/nginx/html/js/index.js

EXPOSE 80

CMD ["nginx", "-g", "daemon off;"]
